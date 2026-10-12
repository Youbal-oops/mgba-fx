/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

/* Execute the generated ROM and verify its guest-written status and screen. */
int main(int argc, char** argv) {
	if (argc != 4 || (strcmp(argv[2], "0") && strcmp(argv[2], "1"))) {
		fprintf(stderr, "Usage: %s ROM EXPECTED_MODE(0|1) OUTPUT.png\n", argv[0]);
		return 2;
	}
	unsigned expected = atoi(argv[2]);
	struct mCore* core = GBACoreCreate();
	if (!core || !core->init(core)) {
		return 1;
	}
	mCoreInitConfig(core, 0);
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
	/* The guest copies one of its two complete original frame assets into VRAM. */
	unsigned index;
	for (index = 0; index < 240 * 160; ++index) {
		if (core->busRead16(core, 0x06000000 + index * 2) !=
		    core->busRead16(core, 0x08000200 + expected * 76800 + index * 2)) {
			fprintf(stderr, "Diagnostic screen mismatch at pixel %u\n", index);
			goto cleanup;
		}
	}
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
