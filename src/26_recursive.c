#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lib/performance.h"
#include "lib/queue.h"
#include "lib/types.h"

/**
 * The inital domain block size is image_size / 2^MIN_QUADTREE_DEPTH
 * Should be >=1, because we can then can assume that the amount of domain
 * blocks is divisible by 2.
 */
#define MIN_QUADTREE_DEPTH 1
#define MAX_QUADTREE_DEPTH 8
#define MIN_RANGE_BLOCK_SIZE 4

static inline void rotate_raw_0(double *out, const double *in, int size) {
    int m = size;
    int n = size;
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < n; ++j) {
            out[i * n + j] = in[i * n + j];
        }
    }
}

static inline void rotate_raw_90(double *out, const double *in, int size) {
    int m = size;
    int n = size;
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < n; ++j) {
            // first row has to be last column
            // (too be honest it was trial and error)
            out[j * n + (m - i - 1)] = in[i * n + j];
        }
    }
}

static inline void rotate_raw_180(double *out, const double *in, int size) {
    int m = size;
    int n = size;
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < n; ++j) {
            // first row has to be last row reversed
            out[(m - i - 1) * n + (n - j - 1)] = in[i * n + j];
        }
    }
}

static inline void rotate_raw_270(double *out, const double *in, int size) {
    int m = size;
    int n = size;
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < n; ++j) {
            // first row has to be first column reversed
            out[(m - j - 1) * n + i] = in[i * n + j];
        }
    }
}

static void load_block(double *ret_out, double *ret_sum, double *ret_sum_squared, const int block_rel_y,
                       const int block_rel_x, const int block_size, const double *image,
                       const int image_size) {
    double sum = 0;
    double sum_squared = 0;

    int idx = 0;
    int idx_in_image = block_rel_y * image_size + block_rel_x;
    for (int i = 0; i < block_size; ++i) {
        for (int j = 0; j < block_size; ++j) {
            double val = image[idx_in_image];
            sum += val;
            sum_squared = fma(val, val, sum_squared);
            ret_out[idx] = val;
            idx++;
            idx_in_image++;
        }
        idx_in_image += image_size - block_size;
    }
    __record_double_flops(block_size * block_size * 3);

    *ret_sum = sum;
    *ret_sum_squared = sum_squared;
}

static inline void scale_block_with_sums(double *out, double *ret_sum, double *ret_sum_squared,
                                         const double *image, const int image_size,
                                         const int idx_db_start_in_image, const int block_size) {
    double sum = 0;
    double sum_squared = 0;
    size_t scaled_image_idx = 0;
    size_t original_image_idx = idx_db_start_in_image;

    for (int y = 0; y < block_size; y += 2) {
        for (int x = 0; x < block_size; x += 2) {
            double val = 0.0;
            val += image[original_image_idx];
            val += image[original_image_idx + 1];
            val += image[original_image_idx + image_size];
            val += image[original_image_idx + image_size + 1];
            val *= 0.25;
            out[scaled_image_idx] = val;
            sum += val;
            sum_squared += val * val;

            scaled_image_idx++;
            original_image_idx += 2;
            __record_double_flops(8);
        }
        original_image_idx += image_size;
    }

    *ret_sum = sum;
    *ret_sum_squared = sum_squared;
}

static void match_block(struct queue *transformations, const unsigned int range_block_idx,
                        const unsigned int range_block_size, const double *image, const int image_size,
                        const unsigned int error_threshold, const int unsigned current_quadtree_depth) {
    const int num_pixels = range_block_size * range_block_size;
    const double num_pixels_of_blocks_inv = 1.0 / num_pixels;
    __record_double_flops(1);

    const int rb_rel_y = BLOCK_CORD_REL_Y(range_block_idx, range_block_size, image_size);
    const int rb_rel_x = BLOCK_CORD_REL_X(range_block_idx, range_block_size, image_size);
    const int rb_start_in_image = rb_rel_y * image_size + rb_rel_x;

    double range_sum = 0;
    double range_sum_squared = 0;
    {  // RANGE SUM, RANGE SUM SQUARED
        int idx = rb_start_in_image;
        for (size_t i = 0; i < range_block_size; ++i) {
            for (int j = 0; j < range_block_size; ++j) {
                double val = image[idx];
                range_sum += val;
                range_sum_squared = fma(val, val, range_sum_squared);
                idx++;
            }
            idx += image_size - range_block_size;
        }
        __record_double_flops(3 * range_block_size * range_block_size);
    }

    const double sr_x_2 = 2 * range_sum;
    __record_double_flops(1);

    double best_error = DBL_MAX;
    int best_domain_block_idx = -1;
    double best_contrast = -1;
    double best_brightness = -1;
    int best_angle = -1;

    const int domain_block_size = 2 * range_block_size;
    const int domain_blocks_length = (image_size / domain_block_size) * (image_size / domain_block_size);

    for (size_t idx_db = 0; idx_db < domain_blocks_length; ++idx_db) {
        double domain_sum, domain_sum_squared;
        double *domain_block = ALLOCATE(sizeof(double) * range_block_size * range_block_size);
        const int db_rel_x = BLOCK_CORD_REL_X(idx_db, domain_block_size, image_size);
        const int db_rel_y = BLOCK_CORD_REL_Y(idx_db, domain_block_size, image_size);
        const int db_start_in_image = db_rel_y * image_size + db_rel_x;

        scale_block_with_sums(domain_block, &domain_sum, &domain_sum_squared, image, image_size,
                              db_start_in_image, domain_block_size);

        const double ds_x_ds = domain_sum * domain_sum;
        const double num_pixels_x_dss = num_pixels * domain_sum_squared;
        const double denominator = num_pixels_x_dss - ds_x_ds;
        __record_double_flops(3);

        if (denominator == 0) {
            double brightness = range_sum * num_pixels_of_blocks_inv;
            __record_double_flops(1);
            double error;
            if (num_pixels == 1) {
                error = 0.0;
            } else {
                error = (range_sum_squared + brightness * (num_pixels * brightness - sr_x_2)) *
                        num_pixels_of_blocks_inv;
                __record_double_flops(5);
            }

            if (error < best_error) {
                best_error = error;
                best_domain_block_idx = idx_db;
                best_contrast = 0.0;
                best_brightness = brightness;
                best_angle = 0;
            }
        }

        const double sd_x_sr = range_sum * domain_sum;
        const double denominator_inv = 1.0 / denominator;
        const double sd_x_2 = 2 * domain_sum;
        __record_double_flops(3);

        // BEGIN precompute rtd
        int rtd_idx_rb = rb_start_in_image;
        int dbs = range_block_size;
        int dbs_dbs = dbs * dbs;

        double rtd_sum_0_1 = 0;
        double rtd_sum_0_2 = 0;
        double rtd_sum_90_1 = 0;
        double rtd_sum_90_2 = 0;
        double rtd_sum_180_1 = 0;
        double rtd_sum_180_2 = 0;
        double rtd_sum_270_1 = 0;
        double rtd_sum_270_2 = 0;

        int dbs_i = 0;
        for (int i = 0; i < dbs; i++) {
            int dbs_j = 0;
            for (int j = 0; j < dbs; j += 2) {
                int idx_0_db1 = dbs_i + j;
                int idx_0_db2 = idx_0_db1 + 1;
                int idx_90_db1 = dbs_dbs - dbs_j - dbs + i;
                int idx_90_db2 = idx_90_db1 - dbs;
                int idx_180_db1 = dbs_dbs - dbs_i - j - 1;
                int idx_180_db2 = idx_180_db1 - 1;
                int idx_270_db1 = dbs_j + dbs - i - 1;
                int idx_270_db2 = idx_270_db1 + dbs;

                int idx_rb2 = rtd_idx_rb + 1;

                double ri1 = image[rtd_idx_rb];
                double ri2 = image[idx_rb2];

                double di_0_1 = domain_block[idx_0_db1];
                double di_0_2 = domain_block[idx_0_db2];
                double di_90_1 = domain_block[idx_90_db1];
                double di_90_2 = domain_block[idx_90_db2];
                double di_180_1 = domain_block[idx_180_db1];
                double di_180_2 = domain_block[idx_180_db2];
                double di_270_1 = domain_block[idx_270_db1];
                double di_270_2 = domain_block[idx_270_db2];

                rtd_sum_0_1 += ri1 * di_0_1;
                rtd_sum_0_2 += ri2 * di_0_2;

                rtd_sum_90_1 += ri1 * di_90_1;
                rtd_sum_90_2 += ri2 * di_90_2;

                rtd_sum_180_1 += ri1 * di_180_1;
                rtd_sum_180_2 += ri2 * di_180_2;

                rtd_sum_270_1 += ri1 * di_270_1;
                rtd_sum_270_2 += ri2 * di_270_2;

                rtd_idx_rb += 2;
                dbs_j = dbs_j + dbs + dbs;
            }
            rtd_idx_rb += image_size - dbs;
            dbs_i += dbs;
        }
        __record_double_flops(dbs * dbs * 8);

        double rtd_0 = rtd_sum_0_1 + rtd_sum_0_2;
        double rtd_90 = rtd_sum_90_1 + rtd_sum_90_2;
        double rtd_180 = rtd_sum_180_1 + rtd_sum_180_2;
        double rtd_270 = rtd_sum_270_1 + rtd_sum_270_2;
        __record_double_flops(4);

        // END precompute rtd

        // ROTATION 0
        {
            double contrast = fma(num_pixels, rtd_0, -sd_x_sr) * denominator_inv;
            double brightness = fma(contrast, -domain_sum, range_sum) * num_pixels_of_blocks_inv;
            double a1 = fma(num_pixels, brightness, -sr_x_2);
            double a2 = fma(brightness, a1, range_sum_squared);
            double a3 = fma(brightness, sd_x_2, -rtd_0);
            double a4 = fma(contrast, domain_sum_squared, -rtd_0);
            double a5 = a3 + a4;
            double error = fma(a5, contrast, a2);
            error *= num_pixels_of_blocks_inv;
            __record_double_flops(18);

            if (error < best_error && contrast < 1.0 && contrast > -1.0) {
                best_error = error;
                best_domain_block_idx = idx_db;
                best_contrast = contrast;
                best_brightness = brightness;
                best_angle = 0;
            }
        }

        // ROTATION 90
        {
            double contrast = fma(num_pixels, rtd_90, -sd_x_sr) * denominator_inv;
            double brightness = fma(contrast, -domain_sum, range_sum) * num_pixels_of_blocks_inv;
            double a1 = fma(num_pixels, brightness, -sr_x_2);
            double a2 = fma(brightness, a1, range_sum_squared);
            double a3 = fma(brightness, sd_x_2, -rtd_90);
            double a4 = fma(contrast, domain_sum_squared, -rtd_90);
            double a5 = a3 + a4;
            double error = fma(a5, contrast, a2);
            error *= num_pixels_of_blocks_inv;
            __record_double_flops(18);

            if (error < best_error && contrast < 1.0 && contrast > -1.0) {
                best_error = error;
                best_domain_block_idx = idx_db;
                best_contrast = contrast;
                best_brightness = brightness;
                best_angle = 90;
            }
        }

        // ROTATION 180
        {
            double contrast = fma(num_pixels, rtd_180, -sd_x_sr) * denominator_inv;
            double brightness = fma(contrast, -domain_sum, range_sum) * num_pixels_of_blocks_inv;
            double a1 = fma(num_pixels, brightness, -sr_x_2);
            double a2 = fma(brightness, a1, range_sum_squared);
            double a3 = fma(brightness, sd_x_2, -rtd_180);
            double a4 = fma(contrast, domain_sum_squared, -rtd_180);
            double a5 = a3 + a4;
            double error = fma(a5, contrast, a2);
            error *= num_pixels_of_blocks_inv;
            __record_double_flops(18);

            if (error < best_error && contrast < 1.0 && contrast > -1.0) {
                best_error = error;
                best_domain_block_idx = idx_db;
                best_contrast = contrast;
                best_brightness = brightness;
                best_angle = 180;
            }
        }

        // ROTATION 270
        {
            double contrast = fma(num_pixels, rtd_270, -sd_x_sr) * denominator_inv;
            double brightness = fma(contrast, -domain_sum, range_sum) * num_pixels_of_blocks_inv;
            double a1 = fma(num_pixels, brightness, -sr_x_2);
            double a2 = fma(brightness, a1, range_sum_squared);
            double a3 = fma(brightness, sd_x_2, -rtd_270);
            double a4 = fma(contrast, domain_sum_squared, -rtd_270);
            double a5 = a3 + a4;
            double error = fma(a5, contrast, a2);
            error *= num_pixels_of_blocks_inv;
            __record_double_flops(18);

            if (error < best_error && contrast < 1.0 && contrast > -1.0) {
                best_error = error;
                best_domain_block_idx = idx_db;
                best_contrast = contrast;
                best_brightness = brightness;
                best_angle = 270;
            }
        }

        free(domain_block);
    }

    int next_range_block_size = range_block_size >> 1;
    if (best_error > error_threshold && current_quadtree_depth < MAX_QUADTREE_DEPTH &&
        next_range_block_size >= MIN_RANGE_BLOCK_SIZE && range_block_size % 2 == 0) {
        const int rb_y = BLOCK_CORD_Y(range_block_idx, range_block_size, image_size);
        const int rb_x = BLOCK_CORD_X(range_block_idx, range_block_size, image_size);
        const int blocks_per_row = image_size / range_block_size;
        int next_id_1 = 4 * rb_y * blocks_per_row + 2 * rb_x;
        int next_id_2 = next_id_1 + 1;
        int next_id_3 = next_id_1 + 2 * blocks_per_row;
        int next_id_4 = next_id_3 + 1;

        match_block(transformations, next_id_1, next_range_block_size, image, image_size, error_threshold,
                    current_quadtree_depth + 1);
        match_block(transformations, next_id_2, next_range_block_size, image, image_size, error_threshold,
                    current_quadtree_depth + 1);
        match_block(transformations, next_id_3, next_range_block_size, image, image_size, error_threshold,
                    current_quadtree_depth + 1);
        match_block(transformations, next_id_4, next_range_block_size, image, image_size, error_threshold,
                    current_quadtree_depth + 1);
    } else {
        struct transformation_t *best_transformation = malloc(sizeof(struct transformation_t));

        best_transformation->range_block = make_block(rb_rel_x, rb_rel_y, range_block_size, range_block_size);

        int db_rel_x = BLOCK_CORD_REL_X(best_domain_block_idx, domain_block_size, image_size);
        int db_rel_y = BLOCK_CORD_REL_Y(best_domain_block_idx, domain_block_size, image_size);

        best_transformation->domain_block =
            make_block(db_rel_x, db_rel_y, domain_block_size, domain_block_size);

        best_transformation->brightness = best_brightness;
        best_transformation->contrast = best_contrast;
        best_transformation->angle = best_angle;
        enqueue(transformations, best_transformation);
    }
}

static struct queue *compress(const struct image_t *image, const int error_threshold) {
    const int initial_domain_block_size = image->size / (int)pow(2.0, (double)MIN_QUADTREE_DEPTH);
    const int initial_range_block_size = initial_domain_block_size / 2;

    const size_t initial_range_blocks_length =
        (image->size / initial_range_block_size) * (image->size / initial_range_block_size);

    // Queue for saving transformations
    struct queue *transformations = (struct queue *)ALLOCATE(sizeof(struct queue));
    *transformations = make_queue();

    for (size_t idx_rb = 0; idx_rb < initial_range_blocks_length; ++idx_rb) {
        match_block(transformations, idx_rb, initial_range_block_size, image->data, image->size,
                    error_threshold, 1);
    }

    return transformations;
}

static void scale_block(double *out, const double *image, const int image_size, const int block_rel_x,
                        const int block_rel_y, const int block_size) {
    size_t scaled_image_idx = 0;
    size_t original_image_idx = block_rel_y * image_size + block_rel_x;

    for (int y = 0; y < block_size; y += 2) {
        for (int x = 0; x < block_size; x += 2) {
            double val = 0.0;
            val += image[original_image_idx];
            val += image[original_image_idx + 1];
            val += image[original_image_idx + image_size];
            val += image[original_image_idx + image_size + 1];
            out[scaled_image_idx] = val * 0.25;
            scaled_image_idx++;
            original_image_idx += 2;
            __record_double_flops(5);
        }
        original_image_idx += image_size;
    }
}

static void apply_transformation(struct image_t *image, const struct transformation_t *t) {
    assert(t->domain_block.width == t->domain_block.height);
    assert(t->range_block.width == t->range_block.height);

    double *scaled_domain_block = ALLOCATE(sizeof(double) * t->range_block.width * t->range_block.height);
    scale_block(scaled_domain_block, image->data, image->size, t->domain_block.rel_x, t->domain_block.rel_y,
                t->domain_block.height);
    double *rotated_domain_block = ALLOCATE(sizeof(double) * t->range_block.height * t->range_block.height);

    switch (t->angle) {
        case 0:
            rotate_raw_0(rotated_domain_block, scaled_domain_block, t->range_block.height);
            break;
        case 90:
            rotate_raw_90(rotated_domain_block, scaled_domain_block, t->range_block.height);
            break;
        case 180:
            rotate_raw_180(rotated_domain_block, scaled_domain_block, t->range_block.height);
            break;
        case 270:
            rotate_raw_270(rotated_domain_block, scaled_domain_block, t->range_block.height);
            break;
        default:
            assert(0);
            break;
    }

    for (int i = 0; i < t->range_block.height; ++i) {
        for (int j = 0; j < t->range_block.width; ++j) {
            double value = rotated_domain_block[i * t->range_block.height + j];
            int idx = get_index_in_image(&t->range_block, i, j, image);
            int new_pixel_value = value * t->contrast + t->brightness;
            if (new_pixel_value < 0) new_pixel_value = 0;
            if (new_pixel_value > 255) new_pixel_value = 255;
            image->data[idx] = new_pixel_value;
            __record_double_flops(2);
        }
    }

    free(scaled_domain_block);
    free(rotated_domain_block);
}

static void decompress(struct image_t *decompressed_image, const struct queue *transformations,
                       const int num_iterations) {
    for (int iter = 0; iter < num_iterations; ++iter) {
        const struct queue_node *current = transformations->front;
        while (current != transformations->back) {
            apply_transformation(decompressed_image, (const struct transformation_t *)current->data);
            current = current->next;
        }
    }
}

struct func_suite_t register_suite(void) {
    struct func_suite_t suite = {.compress_func = &compress, .decompress_func = &decompress};
    return suite;
}
