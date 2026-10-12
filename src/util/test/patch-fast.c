/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"
#include <mgba-util/patch/fast.h>

M_TEST_DEFINE(variableLengthRoundTrips) {
	uint32_t source[80], target[80], restored[80];
	unsigned size, pattern;
	struct PatchFast patch;
	initPatchFast(&patch);
	for (size = 0; size <= 257; ++size) {
		for (pattern = 0; pattern < 3; ++pattern) {
			memset(source, 0xA5, sizeof(source));
			memset(target, 0xA5, sizeof(target));
			memset(restored, 0xA5, sizeof(restored));
			uint8_t* original = (uint8_t*) source + 16;
			uint8_t* next = (uint8_t*) target + 16;
			uint8_t* output = (uint8_t*) restored + 16;
			unsigned i;
			for (i = 0; i < size; ++i) {
				original[i] = i * 37 + 11;
				next[i] = original[i];
				if (!pattern || (pattern == 1 && i % 31 == 0) || (pattern == 2 && i + 5 >= size)) {
					next[i] ^= 0x5A;
				}
			}
			assert_true(diffPatchFast(&patch, original, next, size));
			assert_true(patch.d.applyPatch(&patch.d, original, size, output, size));
			assert_memory_equal(output, next, size);
			for (i = 0; i < 16; ++i) {
				assert_int_equal(((uint8_t*) restored)[i], 0xA5);
				assert_int_equal(output[size + i], 0xA5);
			}
			assert_true(patch.d.applyPatch(&patch.d, next, size, output, size));
			assert_memory_equal(output, original, size);
		}
	}
	deinitPatchFast(&patch);
}

M_TEST_DEFINE(rejectInvalidExtents) {
	struct PatchFast patch;
	initPatchFast(&patch);
	uint8_t source[32] = {0}, target[32] = {0};
	struct PatchFastExtent* extent = PatchFastExtentsAppend(&patch.extents);
	memset(extent, 0, sizeof(*extent));
	extent->offset = SIZE_MAX - 4;
	extent->length = 16;
	assert_false(patch.d.applyPatch(&patch.d, source, 32, target, 32));
	extent->offset = 0;
	extent->length = 16;
	extent = PatchFastExtentsAppend(&patch.extents);
	memset(extent, 0, sizeof(*extent));
	extent->length = 1;
	assert_false(patch.d.applyPatch(&patch.d, source, 32, target, 32));
	deinitPatchFast(&patch);
}

M_TEST_SUITE_DEFINE(PatchFast,
	cmocka_unit_test(variableLengthRoundTrips),
	cmocka_unit_test(rejectInvalidExtents))
