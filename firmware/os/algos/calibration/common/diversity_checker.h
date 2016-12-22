/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//////////////////////////////////////////////////////////////////////////////
/*
 * This function implements a diversity checker and stores diverse vectors into
 * a memory. We assume that the data is located on a sphere, and we use the
 * norm of the difference of two vectors to decide if the vectors are diverse
 * enough:
 *
 * k = norm( v1 - v2 )^2 < Threshold
 *
 * Hence when k < Threshold the data is not stored, because the vectors are too
 * similar. We store diverse vectors in memory and all new incoming vectors
 * are checked against the already stored data points.
 *
 * Furthermore we also check if k > max_distance, since that data is most likely
 * not located on a sphere anymore and indicates a disturbance. Finally we give
 * a "data is full" flag to indicate once the memory is full.
 * The diverse data can be used to improve sphere fit calibrations, ensuring
 * that the sphere is populated enough resulting in better fits.
 *
 * Memory is stored in an array initialized to length of
 * [THREE_AXIS_DATA_DIM * NUM_DIVERSE_VECTORS], this has been done to be
 * compatible with the full sphere fit algorithm.
 *
 * Notice, this function stops to check if data is diverse, once the memory is
 * full. This has been done in order to save processing power.
 */

#ifndef LOCATION_LBS_CONTEXTHUB_NANOAPPS_CALIBRATION_COMMON_DIVERSITY_CHECKER_H_
#define LOCATION_LBS_CONTEXTHUB_NANOAPPS_CALIBRATION_COMMON_DIVERSITY_CHECKER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THREE_AXIS_DATA_DIM (3)   // data is three-dimensional.
#define NUM_DIVERSE_VECTORS (10)  // Storing 10 data points.

// Main data struct.
struct DiversityChecker {
  // Data memory.
  float diverse_data[THREE_AXIS_DATA_DIM * NUM_DIVERSE_VECTORS];

  // Number of data points in the memory.
  size_t num_points;

  // Number of data points that violated the max_distance condition.
  size_t num_max_dist_violations;

  // Threshold value that is used to check k against.
  float threshold;

  // Maximum distance value.
  float max_distance;

  // Data full bit.
  bool data_full;

  // Setup variables for NormQuality check.

  size_t min_num_diverse_vectors;
  size_t max_num_max_distance;
  float var_threshold;
  float max_min_threshold;
};

// Initialization of the function/struct, input:
// threshold -> sets the threshold value, only distances k that are equal
//              or higher than that will be stored.
// max_distance -> sets the maximum allowed distance of k.
// min_num_diverse_vectors -> sets the gate for a minimum number of data points
//                           in the memory
// max_num_max_distance -> sets the value for a max distance violation number
//                         gate.
// var_threshold -> is a threshold value for a Norm variance gate.
// max_min_threshold -> is a value for a gate that rejects Norm variations
//                      that are larger than this number.
void diversityCheckerInit(struct DiversityChecker* diverse_data,
                          float threshold,
                          float max_distance,
                          size_t min_num_diverse_vectors,
                          size_t max_num_max_distance,
                          float var_threshold,
                          float max_min_threshold);

// Resetting the memory and the counters, leaves threshold and max_distance
// as well as the setup variables for NormQuality check untouched.
void diversityCheckerReset(struct DiversityChecker* diverse_data);

// Main function. Tests the data (x,y,z) against the memory if diverse and
// stores it, if so.
void diversityCheckerUpdate(struct DiversityChecker* diverse_data, float x,
                            float y, float z);

// Removing a constant bias from the diverse_data and check if the norm is
// within a defined bound:
// implemented 4 gates
// -> needs a minimum number of data points in the memory
//    (controlled by min_num_divers_vectors).
// -> will return false if maximum number of max_distance is reached
//    (controlled by max_num_max_distance).
// -> norm must be within a var window.
// -> norm must be within a MAX/MIN window.
// Returned value will only be true if all 4 gates are passed.
bool diversityCheckerNormQuality(struct DiversityChecker* diverse_data,
                                 float x_bias,
                                 float y_bias,
                                 float z_bias);

#ifdef __cplusplus
}
#endif

#endif  // LOCATION_LBS_CONTEXTHUB_NANOAPPS_CALIBRATION_COMMON_DIVERSITY_CHECKER_H_