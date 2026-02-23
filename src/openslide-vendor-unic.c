/*
 * Unic (TMAP) support
 */

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"

#include <math.h>

static const char TMAP_EXT[] = ".TMAP";

struct unic_ops_data {
  char *filename;
};

struct image {
  int64_t start_in_file;
  int32_t length;
  int32_t imageno; // used only for cache lookup
  int32_t width;
  int32_t height;
  int refcount;
};

struct tile {
  struct image *image;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct unic_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *)osr->levels[i]);
  }
  g_free(osr->levels);

  // the ops data
  g_free(data->filename);
  g_free(data);
}

static void image_unref(struct image *image) {
  if (!--image->refcount) {
    g_free(image);
  }
}

typedef struct image image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(image, image_unref)

static void tile_free(gpointer data) {
  struct tile *tile = data;
  image_unref(tile->image);
  g_free(tile);
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            int w, int h,
                            GError **err) {
  struct unic_ops_data *data = osr->data;
  bool result = false;

  g_autofree uint32_t *dest = g_malloc(w * h * 4);

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (f == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File is NULL");
    return NULL;
  }

  if (image->length == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Length is zero");
    return NULL;
  }

  if (image->start_in_file && !_openslide_fseek(f, image->start_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Cannot seek to offset: ");
    return NULL;
  }

  char buf[image->length];
  if (!_openslide_fread_exact(f, buf, sizeof(buf), err)) {
    g_prefix_error(err, "Couldn't read tile data");
    return NULL;
  }

  result = _openslide_jpeg_decode_buffer(buf, image->length, dest,
                                         w, h, err);

  if (!result) {
    return NULL;
  }
  return g_steal_pointer(&dest);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED,
                      void *data,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct tile *tile = data;
  bool success = true;

  int iw = tile->image->width;
  int ih = tile->image->height;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level,
                                            tile->image->imageno,
                                            0,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = read_image(osr, tile->image, iw, ih, err);
    if (tiledata == NULL) {
      return false;
    }
    _openslide_cache_put(osr->cache,
                         level, tile->image->imageno, 0,
                         tiledata,
                         iw * ih * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_RGB24,
                                        iw, ih, iw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return success;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops unic_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool unic_tmap_detect(const char *filename G_GNUC_UNUSED,
                             struct _openslide_tifflike *tl,
                             GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, TMAP_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", TMAP_EXT);
    return false;
  }

  // verify existence
  GError *tmp_err = NULL;
  if (!_openslide_fexists(filename, &tmp_err)) {
    if (tmp_err != NULL) {
      g_propagate_prefixed_error(err, tmp_err, "Testing whether file exists: ");
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "File does not exist");
    }
    return false;
  }

  return true;
}

static bool read_le_int32_from_file_with_result(struct _openslide_file *f,
                                                int32_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 4, NULL)) {
    return false;
  }

  *OUT = GINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int32_t read_le_int32_from_file(struct _openslide_file *f) {
  int32_t i;

  if (!read_le_int32_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static bool read_le_int64_from_file_with_result(struct _openslide_file *f,
                                                int64_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 8, NULL)) {
    return false;
  }

  *OUT = GINT64_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int64_t read_le_int64_from_file(struct _openslide_file *f) {
  int64_t i;

  if (!read_le_int64_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static void insert_tile(struct level *l,
                        struct image *image,
                        double pos_x, double pos_y,
                        int tile_x, int tile_y,
                        int tile_w, int tile_h,
                        int zoom_level) {
  // increment image refcount
  image->refcount++;

  // generate tile
  struct tile *tile = g_new0(struct tile, 1);
  tile->image = image;

  // compute offset
  double offset_x = pos_x - (tile_x * l->base.tile_w);
  double offset_y = pos_y - (tile_y * l->base.tile_h);

  // insert
  _openslide_grid_tilemap_add_tile(l->grid,
                                   tile_x, tile_y,
                                   offset_x, offset_y,
                                   tile_w, tile_h,
                                   tile);

  if (!true) {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
            zoom_level, tile_x, tile_y, pos_x, pos_y, offset_x, offset_y);
  }
}

static bool process_tiles_info_from_header(struct _openslide_file *f,
                                           int64_t seek_location,
                                           int zoom_levels,
                                           int total_tile_count,
                                           struct level **levels,
                                           GError **err) {
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t image_number = 0;
  int zoom_level = -1;
  // read all the data into the list
  for (int i = 0; i < total_tile_count; i++) {
    zoom_level = read_le_int32_from_file(f);
    if (zoom_level < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level < 0");
      return false;
    } else if (zoom_level >= zoom_levels) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level >= zoom levels");
      return false;
    }

    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    struct level *l = levels[zoom_level];

    // position in this level
    int32_t pos_x = read_le_int32_from_file(f);
    int32_t pos_y = read_le_int32_from_file(f);
    int32_t tile_w = read_le_int32_from_file(f);
    int32_t tile_h = read_le_int32_from_file(f);
    int64_t offset = read_le_int64_from_file(f);
    int32_t length = read_le_int32_from_file(f);
    if (offset < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "offset < 0");
      return false;
    }
    if (length < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "length < 0");
      return false;
    }

    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }

    // populate the image structure
    g_autoptr(image) image = g_new0(struct image, 1);
    image->start_in_file = offset;
    image->length = length;
    image->imageno = image_number++;
    image->refcount = 1;
    image->width = tile_w;
    image->height = tile_h;

    // start processing 1 image into 1 tile
    // increments image refcount
    insert_tile(l, image,
                pos_x, pos_y,
                pos_x / l->base.tile_w,
                pos_y / l->base.tile_h,
                tile_w, tile_h,
                zoom_level);
  }

  return true;
}


// Determine the actual scan scale based on pixel size at 100x (mm)
// pixel_size_100x_mm Pixel size at 100x, in mm/pixel
// Scan scale (20.0 or 40.0), or 20.0 if cannot determine as default
double determine_scan_scale(float pixel_size_100x_mm) {
    // Convert to micrometers per pixel (at 100x)
    double pixel_size_100x_um = pixel_size_100x_mm * 1000.0;

    // Calculate theoretical pixel sizes at 20x and 40x (μm/pixel)
    double pixel_20x = pixel_size_100x_um * (100.0 / 20.0); // Equivalent to *5
    double pixel_40x = pixel_size_100x_um * (100.0 / 40.0); // Equivalent to *2.5

    // Define reasonable ranges (μm/pixel), can be adjusted based on actual data
    const double min_20 = 0.4;
    const double max_20 = 0.7;
    const double min_40 = 0.1;
    const double max_40 = 0.3;

    int in_20 = (pixel_20x >= min_20 && pixel_20x <= max_20);
    int in_40 = (pixel_40x >= min_40 && pixel_40x <= max_40);

    if (in_20 && !in_40) {
        return 20.0;
    }
    if (in_40 && !in_20) {
        return 40.0;
    }
    if (in_20 && in_40) {
        // Both are within ranges, judge by closest to typical values
        const double target_20 = 0.5;
        const double target_40 = 0.25;
        double diff_20 = fabs(pixel_20x - target_20);
        double diff_40 = fabs(pixel_40x - target_40);
        return (diff_20 < diff_40) ? 20.0 : 40.0;
    }

    // Neither within range, cannot determine
    fprintf(stderr, "Warning: pixel size 20x=%.3f um/px, 40x=%.3f um/px are outside expected ranges.\n",
            pixel_20x, pixel_40x);
    return 20.0;
}

static bool unic_tmap_open(openslide_t *osr, const char *filename,
                           struct _openslide_tifflike *tl G_GNUC_UNUSED,
                           struct _openslide_hash *quickhash1 G_GNUC_UNUSED, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  // read header
  char buf[4];
  if (!_openslide_fread_exact(f, buf, sizeof(buf), err)) {
    g_prefix_error(err, "Couldn't read within header");
    return false;
  }

  if (memcmp(buf, "TMAP", 4) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported file: %.4s", buf);
    return false;
  }

  // read version
  char versionBuf[4];
  if (!_openslide_fread_exact(f, versionBuf, sizeof(versionBuf), err)) {
    g_prefix_error(err, "Couldn't read version within header");
    return false;
  }

  // only support version 7 for now
  if (memcmp(versionBuf, "07", 2) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported file version: %.2s", versionBuf);
    return false;
  }

  // add properties
  if (!_openslide_fseek(f, 8, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  // read pixel size at 100x, in mm/pixel
  unsigned char PixelSizeBuf[4];
  if (!_openslide_fread_exact(f, PixelSizeBuf, sizeof(PixelSizeBuf), err)) {
    g_prefix_error(err, "Couldn't read PixelSize within header");
    return false;
  }

  float *PixelSizeOf100X = (float *) PixelSizeBuf;
  double ScanScale = determine_scan_scale(*PixelSizeOf100X); // scanning scale factor e.g. 20X or 40X

  // set MPP and objective power
  g_hash_table_insert(osr->properties,
                      g_strdup("unic.ScanScale"),
                      _openslide_format_double(ScanScale));
  g_hash_table_insert(osr->properties,
                      g_strdup("unic.PixelSize"),
                      _openslide_format_double(*PixelSizeOf100X * 100000 / ScanScale));

  _openslide_duplicate_double_prop(osr, "unic.ScanScale",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "unic.PixelSize",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "unic.PixelSize",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  int32_t associate_image_count = read_le_int32_from_file(f);
  int32_t zoom_levels = read_le_int32_from_file(f);
  int32_t tile_count = read_le_int32_from_file(f);

  // add associated images
  // read all associated image information entries
  int64_t associated_images_info_in_file = 304;
  if (!_openslide_fseek(f, associated_images_info_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int64_t thumbnail_data_in_file = -1;
  int64_t navigate_data_in_file = -1;
  int64_t macro_data_in_file = -1;
  int64_t label_data_in_file = -1;
  int64_t preview_data_in_file = -1;
  // base dimensions
  int64_t base_h = -1;
  int64_t base_w = -1;
  int32_t tile_size = -1;

  for (int i = 0; i < associate_image_count; i++) {
    int32_t w = read_le_int32_from_file(f);
    int32_t h = read_le_int32_from_file(f);
    int32_t color = read_le_int32_from_file(f);
    int32_t id = read_le_int32_from_file(f);
    g_assert(color == 24);
    int64_t offset = read_le_int64_from_file(f);
    int32_t length = read_le_int32_from_file(f);
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    switch (id) {
    case 0:
      thumbnail_data_in_file = offset;
      break;
    case 1:
      navigate_data_in_file = offset;
      break;
    case 2:
      macro_data_in_file = offset;
      break;
    case 3:
      label_data_in_file = offset;
      break;
    case 4:
      preview_data_in_file = offset;
      break;
    case 5:
      g_assert(w == h);
      tile_size = w;
      break;
    case 6:
      base_w = w;
      base_h = h;
      break;
    }
  }

  g_hash_table_insert(osr->properties,
                      g_strdup("unic.TileSize"),
                      _openslide_format_double((double) tile_size));  

  if (thumbnail_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "thumbnail", filename, thumbnail_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "thumbnail");
      return false;
    }
  }

  if (navigate_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "navigate", filename, navigate_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "navigate");
      return false;
    }
  }

  if (macro_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "macro", filename, macro_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "macro");
      return false;
    }
  }

  if (label_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "label", filename, label_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "label");
      return false;
    }
  }

  if (preview_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "preview", filename, preview_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "preview");
      return false;
    }
  }

  int64_t levels_info_in_file = 564;
  if (!_openslide_fseek(f, levels_info_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  // set up level dimensions and such
  int64_t tiles_info_in_file;
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  int64_t downsample = 1;
  for (int i = 0; i < zoom_levels; i++) {
    // ensure downsample is > 0 and a power of 2
    if (downsample <= 0 || (downsample & (downsample - 1))) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid downsample %" PRId64, downsample);
      return false;
    }

    if (i == 0)
      downsample = 1;
    else
      downsample *= 2;

    struct level *l = g_new0(struct level, 1);
    g_ptr_array_add(level_array, l);

    l->base.downsample = downsample;
    l->base.tile_w = (double) tile_size;
    l->base.tile_h = (double) tile_size;

    l->base.w = base_w / l->base.downsample;
    if (l->base.w == 0)
      l->base.w = 1;
    l->base.h = base_h / l->base.downsample;
    if (l->base.h == 0)
      l->base.h = 1;

    l->grid = _openslide_grid_create_tilemap(osr,
                                             tile_size,
                                             tile_size,
                                             read_tile, tile_free);

    // skip level mark (A0 41, 20 41, ... , 20 3E, A0 3D, ...)
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    int32_t w = read_le_int32_from_file(f);
    int32_t h = read_le_int32_from_file(f);
    g_assert(w >= l->base.w);
    g_assert(h >= l->base.h);
    int32_t tile_rows = read_le_int32_from_file(f);
    int32_t tile_cols = read_le_int32_from_file(f);
    g_assert(tile_rows >= 1);
    g_assert(tile_cols >= 1);
    int64_t tiles_info_offset = read_le_int64_from_file(f);
    if (i == 0) {
      tiles_info_in_file = tiles_info_offset;
    }
    // skip level id (1, 2, ... , 8, 9, ...)
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
  }

  // load the position map and build up the tiles
  if (!process_tiles_info_from_header(f,
                                      tiles_info_in_file,
                                      zoom_levels,
                                      tile_count,
                                      (struct level **) level_array->pdata,
                                      err)) {
    return false;
  }

  // build ops data
  struct unic_ops_data *data = g_new0(struct unic_ops_data, 1);
  data->filename = g_strdup(filename);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = zoom_levels;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &unic_ops;

  return true;
}

const struct _openslide_format _openslide_format_unic = {
  .name = "unic-tmap",
  .vendor = "unic",
  .detect = unic_tmap_detect,
  .open = unic_tmap_open,
};
