#include <wand/MagickWand.h>

int insert_resized(char *path, int q, int wid, int hei, char *cache) {

	int w, h, factor;
		
	MagickWand *wand = NULL;
	MagickWandGenesis();
	
	wand = NewMagickWand();
	
	if (MagickReadImage(wand, path) == 0) {
		fprintf(stderr, "Error while reading image\n");
		return EXIT_FAILURE;
	}
	
	if (MagickResizeImage(wand, wid, hei, LanczosFilter, 1) == 0) {
		fprintf(stderr, "Error while resizing\n");
		return EXIT_FAILURE;
	}
	if (MagickSetImageCompressionQuality(wand, q) == 0) {
		fprintf(stderr, "Error while compressing\n");
		return EXIT_FAILURE;
	}
	
	if (MagickWriteImage(wand, cache) == 0) {
		fprintf(stderr, "Error while writing image\n");
		return EXIT_FAILURE;
	}
	
	DestroyMagickWand(wand);
	MagickWandTerminus();
	
	return EXIT_SUCCESS;
		
}
