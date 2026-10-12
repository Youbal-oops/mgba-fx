/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/fx.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

/* Execute the generated ROM and verify its guest-written status and screen. */
int main(int argc, char** argv) {
	if ((argc != 4 && argc != 6) || (strcmp(argv[2], "0") && strcmp(argv[2], "1") && strcmp(argv[2], "policy"))) {
		fprintf(stderr, "Usage: %s ROM EXPECTED_MODE(0|1|policy) OUTPUT.png [MEMORY_MIB CPU_PERCENT]\n", argv[0]);
		return 2;
	}
	bool policy = !strcmp(argv[2], "policy");
	unsigned expected = policy ? 1 : atoi(argv[2]);
	struct mCore* core = GBACoreCreate();
	if (!core || !core->init(core)) {
		return 1;
	}
	mCoreInitConfig(core, 0);
	if (argc == 6) {
		mCoreConfigSetValue(&core->config, "gba.fxMemoryMiB", argv[4]);
		mCoreConfigSetValue(&core->config, "gba.fxCPUPercent", argv[5]);
	}
	core->loadConfig(core, &core->config);
	core->opts.skipBios = true;
	core->opts.useBios = false;
	mColor* pixels = calloc(240 * 160, sizeof(*pixels));
	int result = 1;
	if (!pixels) {
		goto cleanup;
	}
	core->setVideoBuffer(core, pixels, 240);
	if (!mCoreLoadFile(core, argv[1])) {
		fprintf(stderr, "Unable to load diagnostic ROM\n");
		goto cleanup;
	}
	core->reset(core);
	unsigned frame;
	for (frame = 0; frame < 8; ++frame) {
		core->runFrame(core);
	}
	unsigned mode = core->busRead32(core, 0x02000000);
	if (mode != expected) {
		fprintf(stderr, "Guest detected %u; expected %u\n", mode, expected);
		goto cleanup;
	}
	uint16_t* reference = calloc(240 * 160, sizeof(*reference));
	if (!reference) {
		goto cleanup;
	}
	unsigned index;
	for (index = 0; index < 240 * 160; ++index) {
		reference[index] = core->busRead16(core, (policy ? 0x08001000 : 0x08000200 + expected * 76800) + index * 2);
	}
	if (policy) {
		const uint32_t registers[] = {0x3C0, 0x3C4, 0x3EC, 0x3D8, 0x3DC, 0x3F0, 0x3F4};
		if (core->busRead32(core, 0x02000020) != 1) {
			fprintf(stderr, "Guest did not finish drawing its resource values\n");
			free(reference);
			goto cleanup;
		}
		unsigned field;
		for (field = 0; field < 7; ++field) {
			uint32_t value = core->busRead32(core, 0x02000004 + field * 4);
			if (value != core->busRead32(core, 0x04000000 + registers[field])) {
				fprintf(stderr, "Guest resource field %u does not match the emulator\n", field);
				free(reference);
				goto cleanup;
			}
			printf("register=%03X value=%u\n", registers[field], value);
			char digits[11];
			snprintf(digits, sizeof(digits), "%010" PRIu32, value);
			unsigned digit, row, column;
			for (digit = 0; digit < 10; ++digit) {
				for (row = 0; row < 7; ++row) {
					for (column = 0; column < 5; ++column) {
						reference[(34 + field * 14 + row) * 240 + 164 + digit * 6 + column] =
							core->busRead16(core, 0x08014000 + (digits[digit] - '0') * 70 + (row * 5 + column) * 2);
					}
				}
			}
		}
		if (argc == 6 && (core->busRead32(core, 0x02000004) != (uint32_t) atoi(argv[4]) ||
		                  core->busRead32(core, 0x02000010) != (uint32_t) atoi(argv[5]))) {
			fprintf(stderr, "Guest did not receive the configured resource overrides\n");
			free(reference);
			goto cleanup;
		}
	}
	for (index = 0; index < 240 * 160; ++index) {
		if (core->busRead16(core, 0x06000000 + index * 2) != reference[index]) {
			fprintf(stderr, "Diagnostic screen mismatch at pixel %u\n", index);
			free(reference);
			goto cleanup;
		}
	}
	free(reference);
	struct VFile* output = VFileOpen(argv[3], O_WRONLY | O_CREAT | O_TRUNC);
	if (!output) {
		goto cleanup;
	}
	bool screenshot = mCoreTakeScreenshotVF(core, output);
	output->close(output);
	if (!screenshot) {
		goto cleanup;
	}
	printf("mode=%u frames=8 pixels=38400 framebuffer_crc32=%08" PRIX32 "\n",
	       mode, doCrc32(pixels, 240 * 160 * sizeof(*pixels)));
	result = 0;
cleanup:
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	free(pixels);
	return result;
}
