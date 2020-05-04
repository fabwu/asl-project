#include <cassert>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <vector>

#include "common.h"
#include "performance.h"
#include "rotate.h"

using namespace std;

static const std::vector<int> ALL_ANGLES = {0, 90, 180, 270};

inline vector<block_t> create_squared_blocks(const int image_size,
                                             const int block_size,
                                             int x_offset = 0,
                                             int y_offset = 0) {
    assert(image_size % block_size == 0);

    const auto num_blocks =
        (image_size / block_size) * (image_size / block_size);
    vector<block_t> blocks;
    blocks.reserve(num_blocks);
    for (int i = 0; i < image_size; i += block_size) {
        for (int j = 0; j < image_size; j += block_size) {
            blocks.emplace_back(j + x_offset, i + y_offset, block_size,
                                block_size);
        }
    }

    return blocks;
}

image_t *scale_block(const image_t &image, const block_t &block, int width,
                     int height) {
    assert(block.width >= width);
    assert(block.height >= height);
    assert(block.width == block.height);  // just for simplicity
    assert(block.width % width == 0);     // just for simplicity
    assert(block.height % height == 0);   // just for simplicity
    assert(width == height);              // just for simplicity

    const int n = block.width / width;
    const auto scaled_data = (double *)malloc(height * width * sizeof(double));
    const auto compression_blocks =
        create_squared_blocks(block.width, n, block.rel_x, block.rel_y);
    int scaled_index = 0;
    for (auto b : compression_blocks) {
        double val = 0.0;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                val += image[b.get_index_in_image(i, j, image)];
                __record_double_flops(1);
            }
        }
        val /= n * n;
        __record_double_flops(2);

        scaled_data[scaled_index] = val;
        scaled_index++;
    }

    return new image_t(scaled_data, width);
}

/**
 * Returns brightness (o) and contrast (s), such that the RMS between the domain
 * image block and the range block is minmal. Mathematically, this is a least
 * squares problem. The Python implementation we based this naive code on uses
 * such an approach.
 *
 * The Fractal Image Compression book uses a bit a different, more analytical
 * approach. It formulates the error as formula with respect to s and o, and
 * then does partial differentiation with respect to the two variables to get a
 * "direct" solution. Details can be found in the book on page 20 and 21.
 *
 */
tuple<double, double, double> compute_brightness_and_contrast_with_error(
    const image_t &image, const image_t &domain_block_image,
    block_t range_block) {
    assert(domain_block_image.size == range_block.height);
    assert(domain_block_image.size == range_block.width);
    assert(range_block.height == range_block.width);

    const int n = range_block.width;
    const int num_pixels = n * n;

    double sum_domain = 0.0;
    double sum_range = 0.0;
    double sum_range_times_domain = 0.0;
    double sum_domain_squared = 0.0;
    double sum_range_squared = 0.0;

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double di = domain_block_image[i * n + j];
            double ri = image[range_block.get_index_in_image(i, j, image)];
            sum_domain += di;
            sum_range += ri;
            sum_range_times_domain += ri * di;
            sum_domain_squared += di * di;
            sum_range_squared += ri * ri;
        }
    }

    __record_double_flops(n * n * 8);

    double denominator =
        (num_pixels * sum_domain_squared - (sum_domain * sum_domain));
    __record_double_flops(3);
    double contrast;
    if (denominator == 0) {
        contrast = 0.0;
    } else {
        contrast =
            (num_pixels * sum_range_times_domain - sum_domain * sum_range) /
            denominator;
        __record_double_flops(4);
    }
    double brightness = (sum_range - contrast * sum_domain) / num_pixels;
    __record_double_flops(3);

    // Directly compute the error
    double error =
        (sum_range_squared +
         contrast * (contrast * sum_domain_squared -
                     2 * sum_range_times_domain + 2 * brightness * sum_domain) +
         brightness * (num_pixels * brightness - 2 * sum_range)) /
        num_pixels;
    __record_double_flops(14);

    return make_tuple(brightness, contrast, error);
}

static void rotate_domain_blocks(const image_t *domain_block,
                                 vector<const image_t *> &result) {
    result[0] = domain_block;

    for (size_t i = 1; i < ALL_ANGLES.size(); ++i) {
        image_t *rotated_domain_block = new image_t(domain_block->size, false);
        rotate(*rotated_domain_block, *domain_block, ALL_ANGLES[i]);
        result[i] = rotated_domain_block;
    }
}

vector<transformation_t> compress(const image_t &image,
                                  const int block_size_domain) {
    // TODO make this a parameter
    const double error_threshold_per_block = 1000.0;

    // Goal: Try to map blocks of size block_size_domain to blocks of size
    // block_size_range

    assert(block_size_domain % 2 == 0);
    const int initial_range_block_size = block_size_domain / 2;
    auto initial_range_blocks =
        create_squared_blocks(image.size, initial_range_block_size);
    const auto domain_blocks =
        create_squared_blocks(image.size, block_size_domain);

    // Need to compress domain_block, such that size(domain_block) ==
    // size(range_block) in order to compare difference! That means that a
    // square of n pixels need to be compressed to one pixel
    std::vector<std::tuple<block_t, std::vector<const image_t *>>>
        prepared_domain_blocks;
    prepared_domain_blocks.reserve(domain_blocks.size());
    for (const auto &domain_block : domain_blocks) {
        const image_t *scaled_domain_block =
            scale_block(image, domain_block, initial_range_block_size,
                        initial_range_block_size);

        vector<const image_t *> angles(4);
        rotate_domain_blocks(scaled_domain_block, angles);

        prepared_domain_blocks.emplace_back(domain_block, angles);
    }

    // The range blocks we start with
    queue<block_t> remaining_range_blocks;
    for (const auto &b : initial_range_blocks) remaining_range_blocks.push(b);

    // Learn mappings from domain blocks to range blocks
    // That is, find a domain block for every range block, such that their
    // difference is minimal
    vector<transformation_t> transformations;
    int current_range_block_size = initial_range_block_size;
    while (!remaining_range_blocks.empty()) {
        const auto range_block = remaining_range_blocks.front();
        remaining_range_blocks.pop();

        // Should hold because the queue is FIFO and handles all
        // range blocks of a size before reaching smaller sizes
        assert(range_block.width == current_range_block_size ||
               range_block.width == current_range_block_size / 2);
        assert(range_block.width == range_block.height);
        if (range_block.width < current_range_block_size) {
            for (auto &domain : prepared_domain_blocks) {
                const block_t &domain_block = get<0>(domain);
                vector<const image_t *> &angles = get<1>(domain);
                const block_t intermediate_block(
                    domain_block.rel_x, domain_block.rel_y,
                    current_range_block_size, current_range_block_size);
                const image_t *scaled_domain_block =
                    scale_block(*angles[0], intermediate_block,
                                range_block.width, range_block.height);

                // Free old space before allocating new
                for (const image_t *old : angles) delete old;

                rotate_domain_blocks(scaled_domain_block, angles);
            }
            current_range_block_size = range_block.width;
        }

        double best_error = numeric_limits<double>::max();
        transformation_t best_transformation;

        for (const auto &domain : prepared_domain_blocks) {
            const block_t &domain_block = get<0>(domain);
            std::vector<const image_t *> rotated_domain_blocks = get<1>(domain);

            for (size_t i = 0; i < ALL_ANGLES.size(); ++i) {
                const image_t *rotated_domain_block = rotated_domain_blocks[i];
                const int angle = ALL_ANGLES[i];

                double brightness, contrast, error;
                std::tie(brightness, contrast, error) =
                    compute_brightness_and_contrast_with_error(
                        image, *rotated_domain_block, range_block);

                if (error < best_error) {
                    best_error = error;
                    best_transformation = {.domain_block = domain_block,
                                           .range_block = range_block,
                                           .contrast = contrast,
                                           .brightness = brightness,
                                           .angle = angle};
                }
            }
        }

        if (best_error > error_threshold_per_block) {
            assert(range_block.width >= 2);
            assert(range_block.height >= 2);

            auto new_blocks = range_block.quad();
            for (auto b : new_blocks) remaining_range_blocks.push(b);
            continue;
        } else {
            transformations.push_back(best_transformation);
        }
    }

    // Free all intermediate values
    for (const auto &domain : prepared_domain_blocks) {
        for (auto domain_block : get<1>(domain)) {
            delete domain_block;
        }
    }

    return transformations;
}

void apply_transformation(image_t &image, const transformation_t &t) {
    assert(t.domain_block.width == t.domain_block.height);
    assert(t.range_block.width == t.range_block.height);

    const image_t *scaled_domain_block = scale_block(
        image, t.domain_block, t.range_block.width, t.range_block.height);
    image_t rotated_domain_block = image_t(t.range_block.height, false);
    rotate(rotated_domain_block, *scaled_domain_block, t.angle);

    for (int i = 0; i < t.range_block.height; ++i) {
        for (int j = 0; j < t.range_block.width; ++j) {
            double value =
                rotated_domain_block[i * rotated_domain_block.size + j];
            int idx = t.range_block.get_index_in_image(i, j, image);
            image[idx] = value * t.contrast + t.brightness;
            __record_double_flops(2);
        }
    }
}

void decompress(image_t &decompressed_image,
                const vector<transformation_t> &transformations,
                const int num_iterations) {
    for (int iter = 0; iter < num_iterations; ++iter) {
        for (const auto t : transformations) {
            apply_transformation(decompressed_image, t);
        }
    }
}

func_suite_t register_suite() {
    func_suite_t suite = {.compress_func = &compress,
                          .decompress_func = &decompress};
    return suite;
}
