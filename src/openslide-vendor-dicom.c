/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022 Benjamin Gilbert
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * DICOM (.dcm) support
 */

#include "openslide-private.h"
#include "openslide-decode-dicom.h"
#include "openslide-decode-jpeg.h"
#include "openslide-hash.h"

#include <glib.h>
#include <math.h>

#include <dicom/dicom.h>

#if 0
#define debug(...) g_debug(__VA_ARGS__)
#else
#define debug(...)
#endif

enum image_format {
  FORMAT_JPEG,
  FORMAT_RGB,
};

struct dicom_file {
  char *filename;

  GMutex lock;
  DcmFilehandle *filehandle;
  DcmDataSet *metadata;
  DcmBOT *bot;
  enum image_format format;
};

struct dicom_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  int64_t tiles_across;
  int64_t tiles_down;

  double pixel_spacing_x;
  double pixel_spacing_y;
  double objective_lens_power;

  int64_t *frame_position;

  struct dicom_file *file;
};

struct associated {
  struct _openslide_associated_image base;
  struct dicom_file *file;
};

// a set of allowed image types for a class of image
struct allowed_types {
  const char *const *const *types;
  int n_types;
};

// the ImageTypes we allow for pyr levels
static const char *const ORIGINAL_TYPES[] = {
  "ORIGINAL", "PRIMARY", "VOLUME", "NONE", NULL
};
static const char *const RESAMPLED_TYPES[] = {
  "DERIVED", "PRIMARY", "VOLUME", "RESAMPLED", NULL
};
static const char *const *const LEVEL_TYPE_STRINGS[] = {
  ORIGINAL_TYPES,
  RESAMPLED_TYPES
};

static const struct allowed_types LEVEL_TYPES = {
  LEVEL_TYPE_STRINGS,
  G_N_ELEMENTS(LEVEL_TYPE_STRINGS)
};

// the ImageTypes we allow for associated images
static const char LABEL_TYPE[] = "LABEL";
static const char OVERVIEW_TYPE[] = "OVERVIEW";
static const char *const LABEL_TYPES[] = {
  "ORIGINAL", "PRIMARY", LABEL_TYPE, "NONE", NULL
};
static const char *const OVERVIEW_TYPES[] = {
  "ORIGINAL", "PRIMARY", OVERVIEW_TYPE, "NONE", NULL
};
static const char *const *const ASSOCIATED_TYPE_STRINGS[] = {
  LABEL_TYPES,
  OVERVIEW_TYPES
};
static const struct allowed_types ASSOCIATED_TYPES = {
  ASSOCIATED_TYPE_STRINGS,
  G_N_ELEMENTS(ASSOCIATED_TYPE_STRINGS)
};

/* The DICOM UIDs and fields we check.
 */
static const char MediaStorageSOPClassUID[] = "MediaStorageSOPClassUID";
static const char VLWholeSlideMicroscopyImageStorage[] =
  "1.2.840.10008.5.1.4.1.1.77.1.6";
static const char ImageType[] = "ImageType";
static const char SeriesInstanceUID[] = "SeriesInstanceUID";
static const char TotalPixelMatrixColumns[] = "TotalPixelMatrixColumns";
static const char TotalPixelMatrixRows[] = "TotalPixelMatrixRows";
static const char Columns[] = "Columns";
static const char Rows[] = "Rows";
static const char SharedFunctionalGroupsSequence[] = "SharedFunctionalGroupsSequence";
static const char PixelMeasuresSequence[] = "PixelMeasuresSequence";
static const char PixelSpacing[] = "PixelSpacing";
static const char SamplesPerPixel[] = "SamplesPerPixel";
static const char PhotometricInterpretation[] = "PhotometricInterpretation";
static const char PlanarConfiguration[] = "PlanarConfiguration";
static const char BitsAllocated[] = "BitsAllocated";
static const char BitsStored[] = "BitsStored";
static const char HighBit[] = "HighBit";
static const char PixelRepresentation[] = "PixelRepresentation";
static const char LossyImageCompression[] = "LossyImageCompression";
static const char LossyImageCompressionMethod[] = "LossyImageCompressionMethod";
static const char ObjectiveLensPower[] = "ObjectiveLensPower";
static const char DimensionOrganizationType[] = "DimensionOrganizationType";
static const char PerFrameFunctionalGroupsSequence[] = "PerFrameFunctionalGroupsSequence";
static const char FrameContentSequence[] = "FrameContentSequence";
static const char DimensionIndexValues[] = "DimensionIndexValues";

static void print_file(struct dicom_file *f G_GNUC_UNUSED) {
  debug("file:" );
  debug("  filename = %s", f->filename);
  debug("  filehandle = %p", f->filehandle);
  debug("  metadata = %p", f->metadata);
  debug("  bot = %p", f->bot);
}

static void print_level(struct dicom_level *l) {
  debug("level:" );
  print_file(l->file);
  debug("  base.downsample = %g", l->base.downsample);
  debug("  base.w = %" PRId64, l->base.w);
  debug("  base.h = %" PRId64, l->base.h);
  debug("  base.tile_w = %" PRId64, l->base.tile_w);
  debug("  base.tile_h = %" PRId64, l->base.tile_h);
  debug("  grid = %p", l->grid);
  debug("  tiles_across = %" PRId64, l->tiles_across);
  debug("  tiles_down = %" PRId64, l->tiles_down);
  debug("  frame_position = %p", l->frame_position);
}

static void print_frame(DcmFrame *frame G_GNUC_UNUSED) {
  debug("value = %p", dcm_frame_get_value(frame));
  debug("length = %u bytes", dcm_frame_get_length(frame));
  debug("rows = %u", dcm_frame_get_rows(frame));
  debug("columns = %u", dcm_frame_get_columns(frame));
  debug("samples per pixel = %u",
        dcm_frame_get_samples_per_pixel(frame));
  debug("bits allocated = %u", dcm_frame_get_bits_allocated(frame));
  debug("bits stored = %u", dcm_frame_get_bits_stored(frame));
  debug("high bit = %u", dcm_frame_get_high_bit(frame));
  debug("pixel representation = %u",
        dcm_frame_get_pixel_representation(frame));
  debug("planar configuration = %u",
        dcm_frame_get_planar_configuration(frame));
  debug("photometric interpretation = %s",
        dcm_frame_get_photometric_interpretation(frame));
  debug("transfer syntax uid = %s",
        dcm_frame_get_transfer_syntax_uid(frame));
}

static bool dicom_detect(const char *filename,
                         struct _openslide_tifflike *tl,
                         GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // should be able to open as a DICOM
  g_autoptr(DcmFilehandle) filehandle = _openslide_dicom_open(filename, err);
  if (!filehandle) {
    return false;
  }

  DcmError *dcm_error = NULL;
  g_autoptr(DcmDataSet) meta =
    dcm_filehandle_read_file_meta(&dcm_error, filehandle);
  if (!meta) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return false;
  }

  return true;
}

static void dicom_file_destroy(struct dicom_file *f) {
  dcm_filehandle_destroy(f->filehandle);
  dcm_dataset_destroy(f->metadata);
  dcm_bot_destroy(f->bot);
  g_mutex_clear(&f->lock);
  g_free(f->filename);
  g_free(f);
}

typedef struct dicom_file dicom_file;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(dicom_file, dicom_file_destroy)

static bool get_tag_int(DcmDataSet *dataset,
                        const char *keyword,
                        int64_t *result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_integer(NULL, element, 0, result);
}

static bool get_tag_str(DcmDataSet *dataset,
                        const char *keyword,
                        int index,
                        const char **result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_string(NULL, element, index, result);
}

static bool get_tag_decimal_str(DcmDataSet *dataset,
                                const char *keyword,
                                int index,
                                double *result) {
  const char *value_ptr;
  if (!get_tag_str(dataset, keyword, index, &value_ptr)) {
    return false;
  }
  const double value = _openslide_parse_double(value_ptr);
  if (isnan(value)) {
    return false;
  }
  *result = value;
  return true;
}

static bool get_tag_seq(DcmDataSet *dataset,
                        const char *keyword,
                        DcmSequence **result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_sequence(NULL, element, result);
}

static bool get_tag_seq_item(DcmDataSet *dataset,
                             const char *keyword,
                             uint32_t index,
                             DcmDataSet **result) {
  DcmSequence *seq;
  if (!get_tag_seq(dataset, keyword, &seq)) {
    return false;
  }
  *result = dcm_sequence_get(NULL, seq, index);
  return *result != NULL;
}

static char **get_tag_strv(DcmDataSet *dataset,
                           const char *keyword,
                           int length) {
  g_auto(GStrv) a = g_new0(char *, length + 1);
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  if (!element) {
    return NULL;
  }
  for (int i = 0; i < length; i++) {
    const char *item;
    if (!dcm_element_get_value_string(NULL, element, i, &item)) {
      return NULL;
    }
    a[i] = g_strdup(item);
  }
  return g_steal_pointer(&a);
}

static bool verify_tag_int(DcmDataSet *dataset,
                           const char *keyword,
                           int64_t expected_value) {
  int64_t value;
  return get_tag_int(dataset, keyword, &value) &&
         value == expected_value;
}

static bool verify_tag_str(DcmDataSet *dataset,
                           const char *keyword,
                           const char *expected_value) {
  const char *value;
  return get_tag_str(dataset, keyword, 0, &value) &&
         g_str_equal(value, expected_value);
}

static struct dicom_file *dicom_file_new(const char *filename, GError **err) {
  g_autoptr(dicom_file) f = g_new0(struct dicom_file, 1);

  f->filename = g_strdup(filename);
  f->filehandle = _openslide_dicom_open(filename, err);
  if (!f->filehandle) {
    return NULL;
  }

  g_mutex_init(&f->lock);

  DcmError *dcm_error = NULL;
  g_autoptr(DcmDataSet) meta =
    dcm_filehandle_read_file_meta(&dcm_error, f->filehandle);
  if (!meta) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return NULL;
  }

  const char *sop;
  if (!get_tag_str(meta, MediaStorageSOPClassUID, 0, &sop) ||
      !g_str_equal(sop, VLWholeSlideMicroscopyImageStorage)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a WSI DICOM");
    return NULL;
  }

  f->metadata = dcm_filehandle_read_metadata(&dcm_error, f->filehandle);
  if (!f->metadata) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return NULL;
  }

  return g_steal_pointer(&f);
}

static void level_destroy(struct dicom_level *l) {
  _openslide_grid_destroy(l->grid);
  if (l->file) {
    dicom_file_destroy(l->file);
  }
  g_free(l->frame_position);
  g_free(l);
}

typedef struct dicom_level dicom_level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(dicom_level, level_destroy)

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    level_destroy((struct dicom_level *) osr->levels[i]);
  }
  g_free(osr->levels);
}

static void rgb_to_cairo(const uint8_t *rgb, uint32_t *dest,
                         int64_t width, int64_t height) {
  int64_t n_pixels = width * height;
  for (int64_t i = 0; i < n_pixels; i++) {
    dest[i] = 0xff000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2];
    rgb += 3;
  }
}

static bool decode_frame(struct dicom_file *file, int64_t frame_number,
                         uint32_t *dest, int64_t w, int64_t h,
                         GError **err) {
  g_mutex_lock(&file->lock);
  DcmError *dcm_error = NULL;
  g_autoptr(DcmFrame) frame = dcm_filehandle_read_frame(&dcm_error,
                                                        file->filehandle,
                                                        file->metadata,
                                                        file->bot,
                                                        frame_number);
  g_mutex_unlock(&file->lock);

  if (!frame) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return false;
  }

  const void *frame_value = dcm_frame_get_value(frame);
  uint32_t frame_length = dcm_frame_get_length(frame);
  uint32_t frame_width = dcm_frame_get_columns(frame);
  uint32_t frame_height = dcm_frame_get_rows(frame);
  if (frame_width != w || frame_height != h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected image size: %ux%u != %"PRId64"x%"PRId64,
                frame_width, frame_height, w, h);
    return false;
  }

  print_file(file);
  print_frame(frame);

  switch (file->format) {
  case FORMAT_JPEG:
    return _openslide_jpeg_decode_buffer(frame_value, frame_length,
                                         dest, w, h, err);
  case FORMAT_RGB:
    if (frame_length != w * h * 3) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "RGB frame length %u != %"PRIu64, frame_length, w * h * 3);
      return false;
    }
    rgb_to_cairo(frame_value, dest, w, h);
  }
  return true;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct dicom_level *l = (struct dicom_level *) level;

  debug("read_tile: tile_col = %" PRIu64 ", tile_row = %" PRIu64,
        tile_col, tile_row);
  debug("read_tile level:");
  print_level(l);

  int64_t frame_index = tile_col + l->tiles_across * tile_row;
  int64_t frame_number = frame_index + 1;
  if (l->frame_position) {
    frame_number = l->frame_position[frame_index];
    if (frame_number == -1) {
      // missing tile
      return true;
    }
  }

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_autofree uint32_t *buf = g_malloc(l->base.tile_w * l->base.tile_h * 4);
    if (!decode_frame(l->file, frame_number,
                      buf, l->base.tile_w, l->base.tile_h,
                      err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_clip_tile(buf,
                              l->base.tile_w, l->base.tile_h,
                              l->base.w - tile_col * l->base.tile_w,
                              l->base.h - tile_row * l->base.tile_h,
                              err)) {
      return false;
    }

    // put it in the cache
    tiledata = g_steal_pointer(&buf);
    _openslide_cache_put(osr->cache,
                         level, tile_col, tile_row,
                         tiledata, l->base.tile_w * l->base.tile_h * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        l->base.tile_w, l->base.tile_h,
                                        l->base.tile_w * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED,
                         cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct dicom_level *l = (struct dicom_level *) level;

  debug("paint_region: x = %" PRId64 ", y = %" PRId64 ", w = %d, h = %d",
        x, y, w, h);
  debug("paint_region level:");
  print_level(l);

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops dicom_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

// replace with g_strv_equal() once we have glib 2.60
static bool strv_equal(const char *const *a, const char *const *b) {
  while (1) {
    if (!*a && !*b) {
      return true;
    }
    if (!*a || !*b || !g_str_equal(*a, *b)) {
      return false;
    }
    a++;
    b++;
  }
}

static bool is_type(char **type, const struct allowed_types *types) {
  // ImageType must be one of the combinations we accept
  for (int i = 0; i < types->n_types; i++) {
    if (strv_equal(types->types[i], (const char *const *) type)) {
      return true;
    }
  }
  return false;
}

static bool associated_get_argb_data(struct _openslide_associated_image *img,
                                     uint32_t *dest,
                                     GError **err) {
  struct associated *a = (struct associated *) img;
  return decode_frame(a->file, 1, dest, a->base.w, a->base.h, err);
}

static void _associated_destroy(struct associated *a) {
  if (a->file) {
    dicom_file_destroy(a->file);
  }
  g_free(a);
}

typedef struct associated associated;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(associated, _associated_destroy)

static void associated_destroy(struct _openslide_associated_image *img) {
  struct associated *a = (struct associated *) img;
  _associated_destroy(a);
}

static const struct _openslide_associated_image_ops dicom_associated_ops = {
  .get_argb_data = associated_get_argb_data,
  .destroy = associated_destroy,
};

static bool get_format(DcmDataSet *metadata, enum image_format *format,
                       GError **err) {
  if (verify_tag_int(metadata, SamplesPerPixel, 3) &&
      // JPEG can be YCbCr or RGB
      (verify_tag_str(metadata, PhotometricInterpretation, "YBR_FULL_422") ||
       verify_tag_str(metadata, PhotometricInterpretation, "RGB")) &&
      verify_tag_int(metadata, PlanarConfiguration, 0) &&
      verify_tag_int(metadata, BitsAllocated, 8) &&
      verify_tag_int(metadata, BitsStored, 8) &&
      verify_tag_int(metadata, HighBit, 7) &&
      verify_tag_int(metadata, PixelRepresentation, 0) &&
      verify_tag_str(metadata, LossyImageCompression, "01") &&
      verify_tag_str(metadata, LossyImageCompressionMethod, "ISO_10918_1")) {
    *format = FORMAT_JPEG;
    return true;
  } else if (verify_tag_int(metadata, SamplesPerPixel, 3) &&
      verify_tag_str(metadata, PhotometricInterpretation, "RGB") &&
      verify_tag_int(metadata, PlanarConfiguration, 0) &&
      verify_tag_int(metadata, BitsAllocated, 8) &&
      verify_tag_int(metadata, BitsStored, 8) &&
      verify_tag_int(metadata, HighBit, 7) &&
      verify_tag_int(metadata, PixelRepresentation, 0) &&
      verify_tag_str(metadata, LossyImageCompression, "00")) {
    *format = FORMAT_RGB;
    return true;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported frame format");
    return false;
  }
}

// unconditionally takes ownership of dicom_file
static bool add_associated(openslide_t *osr,
                           struct dicom_file *f,
                           char **image_type,
                           GError **err) {
  g_autoptr(associated) a = g_new0(struct associated, 1);
  a->base.ops = &dicom_associated_ops;
  a->file = f;

  // dimensions
  if (!get_tag_int(f->metadata, TotalPixelMatrixColumns, &a->base.w) ||
      !get_tag_int(f->metadata, TotalPixelMatrixRows, &a->base.h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read associated image dimensions");
    return false;
  }

  // associated image name
  char *name;
  if (g_str_equal(image_type[2], LABEL_TYPE)) {
    name = "label";
  } else if (g_str_equal(image_type[2], OVERVIEW_TYPE)) {
    name = "macro";
  } else {
    // is_type() let something unexpected through
    g_assert_not_reached();
  }

  // add
  g_hash_table_insert(osr->associated_images,
                      g_strdup(name),
                      g_steal_pointer(&a));
  return true;
}

// unconditionally takes ownership of dicom_file
static bool add_level(openslide_t *osr,
                      GPtrArray *level_array,
                      struct dicom_file *f,
                      GError **err) {
  g_autoptr(dicom_level) l = g_new0(struct dicom_level, 1);
  l->file = f;

  // dimensions
  if (!get_tag_int(f->metadata, TotalPixelMatrixColumns, &l->base.w) ||
      !get_tag_int(f->metadata, TotalPixelMatrixRows, &l->base.h) ||
      !get_tag_int(f->metadata, Columns, &l->base.tile_w) ||
      !get_tag_int(f->metadata, Rows, &l->base.tile_h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read level dimensions");
    return false;
  }
  l->tiles_across = (l->base.w / l->base.tile_w) + !!(l->base.w % l->base.tile_w);
  l->tiles_down = (l->base.h / l->base.tile_h) + !!(l->base.h % l->base.tile_h);

  // read PixelSpacing to expose as the mpp settings, if present
  DcmDataSet *shared_functional_group;
  DcmDataSet *pixel_measures;
  if (get_tag_seq_item(f->metadata,
                       SharedFunctionalGroupsSequence,
                       0,
                       &shared_functional_group) &&
      get_tag_seq_item(shared_functional_group,
                       PixelMeasuresSequence,
                       0,
                       &pixel_measures)) {
    get_tag_decimal_str(pixel_measures, PixelSpacing, 0, &l->pixel_spacing_x);
    get_tag_decimal_str(pixel_measures, PixelSpacing, 1, &l->pixel_spacing_y);
  }
  // objective power
  get_tag_decimal_str(f->metadata, ObjectiveLensPower, 0, &l->objective_lens_power);

  // grid
  l->grid = _openslide_grid_create_simple(osr,
                                          l->tiles_across, l->tiles_down,
                                          l->base.tile_w, l->base.tile_h,
                                          read_tile);

  // organization
  const char *type;
  if (!get_tag_str(f->metadata, DimensionOrganizationType, 0, &type)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Missing DimensionOrganizationType");
    return false;
  }
  if (g_str_equal(type, "TILED_FULL")) {
    // used by eg. 3dhistech ... frames are in row-major order and we
    // can simply use the BOT
  } else if (g_str_equal(type, "3D") || g_str_equal(type, "TILED_SPARSE")) {
    // used by eg. leica ... we need to read the position of every tile
    l->frame_position = g_new(int64_t, l->tiles_across * l->tiles_down);
    for (int64_t frame_number = 0;
         frame_number < l->tiles_across * l->tiles_down;
         frame_number++) {
      l->frame_position[frame_number] = -1;
    }

    DcmSequence *per_frame_functional_groups_seq;
    if (!get_tag_seq(f->metadata, PerFrameFunctionalGroupsSequence,
                     &per_frame_functional_groups_seq)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Missing PerFrameFunctionalGroupsSequence");
      return false;
    }

    uint32_t n_items = dcm_sequence_count(per_frame_functional_groups_seq);
    for (uint32_t i = 0; i < n_items; i++) {
      DcmError *dcm_error = NULL;
      DcmDataSet *per_frame_functional_group =
        dcm_sequence_get(&dcm_error, per_frame_functional_groups_seq, i);
      if (!per_frame_functional_group) {
        _openslide_dicom_propagate_error(err, dcm_error);
        return false;
      }

      DcmDataSet *dimension_index_vals;
      if (!get_tag_seq_item(per_frame_functional_group,
                            FrameContentSequence, 0,
                            &dimension_index_vals)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Missing FrameContentSequence");
        return false;
      }

      uint32_t tag = dcm_dict_tag_from_keyword(DimensionIndexValues);
      int64_t col, row;
      DcmElement *values =
        dcm_dataset_get(&dcm_error, dimension_index_vals, tag);
      if (!values ||
          !dcm_element_get_value_integer(&dcm_error, values, 0, &col) ||
          !dcm_element_get_value_integer(&dcm_error, values, 1, &row)) {
        _openslide_dicom_propagate_error(err, dcm_error);
        return false;
      }

      // DICOM uses 1-based indexing, we want 0 based
      // frame number i is the tile at (col, row)
      if (col < 1 || row < 1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Invalid frame position (%"PRId64", %"PRId64")", col, row);
        return false;
      }
      int64_t frame_index = (col - 1) + (row - 1) * l->tiles_across;
      if (l->frame_position[frame_index] != -1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Duplicate frame (%"PRId64", %"PRId64")", col, row);
        return false;
      }
      l->frame_position[frame_index] = i + 1;
    }
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported DimensionOrganisationType");
    return false;
  }

  // add
  g_ptr_array_add(level_array, g_steal_pointer(&l));
  return true;
}

// unconditionally takes ownership of dicom_file
static bool maybe_add_file(openslide_t *osr,
                           GPtrArray *level_array,
                           struct dicom_file *file,
                           GError **err) {
  g_autoptr(dicom_file) f = file;

  // check ImageType
  g_auto(GStrv) image_type = get_tag_strv(f->metadata, ImageType, 4);
  if (!image_type) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't get ImageType");
    return false;
  }
  bool is_level = is_type(image_type, &LEVEL_TYPES);
  bool is_associated = is_type(image_type, &ASSOCIATED_TYPES);
  if (!is_level && !is_associated) {
    // unknown type; ignore
    return true;
  }

  // check the image format
  if (!get_format(f->metadata, &f->format, err)) {
    return false;
  }

  // read BOT
  f->bot = dcm_filehandle_read_bot(NULL, f->filehandle, f->metadata);
  if (!f->bot) {
    // unable to read BOT, try to build it instead
    DcmError *dcm_error = NULL;
    f->bot = dcm_filehandle_build_bot(&dcm_error, f->filehandle, f->metadata);
    if (!f->bot) {
      _openslide_dicom_propagate_error(err, dcm_error);
      g_prefix_error(err, "Building BOT: ");
      return false;
    }
  }

  // add
  if (is_level) {
    return add_level(osr, level_array, g_steal_pointer(&f), err);
  } else {
    return add_associated(osr, g_steal_pointer(&f), image_type, err);
  }
}

static void add_properties(openslide_t *osr, struct dicom_level *level0) {
  // pixel spacing is in mm, so convert to microns
  if (level0->pixel_spacing_x && level0->pixel_spacing_y) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                        _openslide_format_double(1000.0 * level0->pixel_spacing_x));
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                        _openslide_format_double(1000.0 * level0->pixel_spacing_y));
  }
  if (level0->objective_lens_power) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER),
                        _openslide_format_double(level0->objective_lens_power));
  }
}

static gint compare_level_width(const void *a, const void *b) {
  const struct dicom_level *aa = *((const struct dicom_level **) a);
  const struct dicom_level *bb = *((const struct dicom_level **) b);

  return bb->base.w - aa->base.w;
}

static bool dicom_open(openslide_t *osr,
                       const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1,
                       GError **err) {
  g_autofree char *dirname = g_path_get_dirname(filename);
  g_autofree char *basename = g_path_get_basename(filename);

  g_autoptr(_openslide_dir) dir = _openslide_dir_open(dirname, err);
  if (!dir) {
    return false;
  }

  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_full(10, (GDestroyNotify) level_destroy);

  // open the passed-in file and get the slide-id
  g_autoptr(dicom_file) start = dicom_file_new(filename, err);
  if (!start) {
    return false;
  }

  const char *tmp;
  if (!get_tag_str(start->metadata, SeriesInstanceUID, 0, &tmp)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "SeriesInstanceUID not found");
    return false;
  }
  g_autofree char *slide_id = g_strdup(tmp);

  if (!maybe_add_file(osr, level_array, g_steal_pointer(&start), err)) {
    g_prefix_error(err, "Reading %s: ", filename);
    return false;
  }

  // scan for other DICOMs with this slide id
  const char *name;
  while ((name = _openslide_dir_next(dir))) {
    // no need to add the start file again
    if (g_str_equal(name, basename)) {
      continue;
    }

    g_autofree char *path = g_build_filename(dirname, name, NULL);

    debug("trying to open: %s ...", path);
    GError *tmp_err = NULL;
    g_autoptr(dicom_file) f = dicom_file_new(path, &tmp_err);
    if (!f) {
      if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
        g_message("opening %s: %s", path, tmp_err->message);
      }
      g_error_free(tmp_err);
      continue;
    }

    const char *this_slide_id;
    if (!get_tag_str(f->metadata, SeriesInstanceUID, 0, &this_slide_id) ||
        !g_str_equal(this_slide_id, slide_id)) {
      if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
        g_message("opening %s: slide ID %s != %s", path, this_slide_id, slide_id);
      }
      continue;
    }

    if (!maybe_add_file(osr, level_array, g_steal_pointer(&f), err)) {
      g_prefix_error(err, "Reading %s: ", path);
      return false;
    }
  }

  if (level_array->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No pyramid levels found");
    return false;
  }

  // sort levels by width
  g_ptr_array_sort(level_array, compare_level_width);

  debug("found levels:");
  for (guint i = 0; i < level_array->len; i++) {
    struct dicom_level *l = (struct dicom_level *) level_array->pdata[i];
    print_level(l);
  }

  add_properties(osr, level_array->pdata[0]);

  // no quickhash yet; disable
  _openslide_hash_disable(quickhash1);

  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->ops = &dicom_ops;

  return true;
}

const struct _openslide_format _openslide_format_dicom = {
  .name = "dicom",
  .vendor = "dicom",
  .detect = dicom_detect,
  .open = dicom_open,
};
