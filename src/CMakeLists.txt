FIND_PACKAGE(Boost COMPONENTS program_options REQUIRED)
INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIRS})

add_library(lib_commons
        lib/common.hpp
        lib/filehandler.hpp
        lib/filehandler.cpp
        lib/tsc_x86.h
        lib/performance.h
        lib/performance.c
        )
add_library(lib_types lib/types.h lib/types.c)
add_library(lib_rotate lib/rotate.h lib/rotate.c)
add_library(lib_queue lib/queue.h lib/queue.c)
add_library(lib_naive naive.c) # baseline
add_library(lib_fast fast.c) # working file
add_library(lib_25 25_ilp.c)
# add_library(lib_27 27_ilp_other_images.c)
add_library(lib_30 30_simd_precomp_rotations.c)
add_library(lib_31 31_simd_precomp_rotations_no_bac_simd.c)
add_library(lib_35 35_simd.c)
add_library(lib_36 36_partial_simd.c)
add_library(lib_37 37_full_simd.c)
# add_library(lib_38 38_simd_other_images) # like 35 but other recursion termination criterion
add_library(lib_40 40_ilp_norot_90_270.c)
add_library(lib_41 41_simd_norot_90_270.c)

add_library(lib_51 51_simd_improved_rot.c)

##################################################################################

add_executable(test_rotate exec/test_rotate.cpp)
add_executable(0_baseline exec/fic.cpp)
add_executable(fic exec/fic.cpp)
add_executable(25_ilp exec/fic.cpp)
# add_executable(27_ilp_other_images exec/fic.cpp)
add_executable(30_simd_precomp_rotations exec/fic.cpp)
add_executable(31_simd_precomp_rotations_no_bac_simd exec/fic.cpp)
add_executable(35_simd exec/fic.cpp)
add_executable(36_partial_simd exec/fic.cpp)
add_executable(37_full_simd exec/fic.cpp)
# add_executable(38_simd_other_images exec/fic.cpp)
add_executable(40_ilp_norot_90_270 exec/fic.cpp)
add_executable(41_simd_norot_90_270 exec/fic.cpp)

add_executable(51_simd_improved_rot exec/fic.cpp)

##################################################################################

target_link_libraries(lib_commons PRIVATE lib_types lib_queue)
target_link_libraries(lib_rotate PRIVATE lib_types)
target_link_libraries(lib_naive PRIVATE lib_types lib_commons lib_queue lib_rotate)
target_link_libraries(lib_fast PRIVATE lib_types lib_commons lib_queue lib_rotate)
target_link_libraries(lib_25 PRIVATE lib_types lib_commons lib_queue lib_rotate)
# target_link_libraries(lib_27 PRIVATE lib_types lib_commons lib_queue lib_rotate)
target_link_libraries(lib_30 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_31 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_35 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_36 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_37 PRIVATE lib_types lib_commons lib_queue)
# target_link_libraries(lib_38 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_40 PRIVATE lib_types lib_commons lib_queue)
target_link_libraries(lib_41 PRIVATE lib_types lib_commons lib_queue)

target_link_libraries(lib_51 PRIVATE lib_types lib_commons lib_queue)

##################################################################################

target_link_libraries(test_rotate PUBLIC lib_rotate)

target_link_libraries(fic PUBLIC lib_types lib_queue lib_commons lib_rotate lib_fast ${Boost_LIBRARIES})
target_link_libraries(0_baseline PUBLIC lib_types lib_queue lib_commons lib_rotate lib_naive ${Boost_LIBRARIES})
target_link_libraries(25_ilp PUBLIC lib_25 ${Boost_LIBRARIES})
# target_link_libraries(27_ilp_other_images PUBLIC lib_27 ${Boost_LIBRARIES})
target_link_libraries(30_simd_precomp_rotations PUBLIC lib_30 ${Boost_LIBRARIES})
target_link_libraries(31_simd_precomp_rotations_no_bac_simd PUBLIC lib_31 ${Boost_LIBRARIES})
target_link_libraries(35_simd PUBLIC lib_35 ${Boost_LIBRARIES})
target_link_libraries(36_partial_simd PUBLIC lib_36 ${Boost_LIBRARIES})
target_link_libraries(37_full_simd PUBLIC lib_37 ${Boost_LIBRARIES})
# target_link_libraries(38_simd_other_images PUBLIC lib_38 ${Boost_LIBRARIES})
target_link_libraries(40_ilp_norot_90_270 PUBLIC lib_40 ${Boost_LIBRARIES})
target_link_libraries(41_simd_norot_90_270 PUBLIC lib_41 ${Boost_LIBRARIES})

target_link_libraries(51_simd_improved_rot PUBLIC lib_51 ${Boost_LIBRARIES})
