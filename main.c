#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <math.h>

typedef struct {
	int width;
	int height;
	int maxval;
} PPMHeader;

static int read_ppm_header(FILE* fp, PPMHeader* header) {
	char buf[16];

	if (!fgets(buf, sizeof(buf), fp)) {
		perror("Failed to read PPM magic number");
		return -1;
	}
	if (strncmp(buf, "P6", 2) != 0) {
		fprintf(stderr, "Invalid PPM format (must be P6)\n");
		return -1;
	}

	do {
		if (!fgets(buf, sizeof(buf), fp)) {
			perror("Failed to read PPM header");
			return -1;
		}
	}
	while (buf[0] == '#');

	if (sscanf(buf, "%d %d", &header->width, &header->height) != 2) {
		fprintf(stderr, "Failed to parse PPM dimensions\n");
		return -1;
	}
	if (!fgets(buf, sizeof(buf), fp) || sscanf(buf, "%d", &header->maxval) != 1) {
		fprintf(stderr, "Failed to read PPM maxval\n");
		return -1;
	}

	if (header->maxval != 255) {
		fprintf(stderr, "Unsupported PPM maxval (must be 255)\n");
		return -1;
	}

	return 0;
}

void draw_progress_bar(uint8_t *fb_data, struct fb_fix_screeninfo finfo, struct fb_var_screeninfo vinfo, int bar_width, int bar_height, int progress) {
	int screen_width = vinfo.xres;
	int screen_height = vinfo.yres;
	int bytes_per_pixel = vinfo.bits_per_pixel / 8;

	int bar_x = (screen_width - bar_width) / 2;
	int bar_y = screen_height - bar_height - 10;

	int corner_radius = (bar_height / 2) + 1;

	for (int y = bar_y; y < bar_y + bar_height; y++) {
		for (int x = bar_x; x < bar_x + bar_width; x++) {
			int dx = 0, dy = 0;
			float distance;
			float alpha = 1.0f;

			// Check for rounded corner regions (adjusting X by 1px outward)
			if (x - bar_x < corner_radius && y - bar_y < corner_radius) { 
				dx = x - (bar_x + corner_radius - 1);
				dy = y - (bar_y + corner_radius - 1);
			}
			else if (x - bar_x >= bar_width - corner_radius && y - bar_y < corner_radius) {
				dx = x - (bar_x + bar_width - corner_radius);
				dy = y - (bar_y + corner_radius - 1);
			}
			else if (x - bar_x < corner_radius && y - bar_y >= bar_height - corner_radius) {
				dx = x - (bar_x + corner_radius - 1);
				dy = y - (bar_y + bar_height - corner_radius);
			}
			else if (x - bar_x >= bar_width - corner_radius && y - bar_y >= bar_height - corner_radius) {
				dx = x - (bar_x + bar_width - corner_radius);
				dy = y - (bar_y + bar_height - corner_radius);
			}
			else {
				dx = dy = 0;
			}

			distance = sqrtf(dx * dx + dy * dy);
			if (distance > corner_radius) continue;

			if (distance > corner_radius - 1) 
				alpha = 1.0f - (distance - (corner_radius - 1));

			// Determine progress color
			uint32_t fg_color = (x - bar_x < bar_width * progress / 100.0) ? 0xFFFFFF : 0x808080;
			uint32_t bg_color = 0x000000;

			// Blend colors for anti-aliasing
			uint8_t r_fg = (fg_color >> 16) & 0xFF;
			uint8_t g_fg = (fg_color >> 8) & 0xFF;
			uint8_t b_fg = fg_color & 0xFF;

			uint8_t r_bg = (bg_color >> 16) & 0xFF;
			uint8_t g_bg = (bg_color >> 8) & 0xFF;
			uint8_t b_bg = bg_color & 0xFF;

			uint8_t r = (uint8_t)(r_fg * alpha + r_bg * (1.0f - alpha));
			uint8_t g = (uint8_t)(g_fg * alpha + g_bg * (1.0f - alpha));
			uint8_t b = (uint8_t)(b_fg * alpha + b_bg * (1.0f - alpha));

			uint32_t blended_pixel = (r << 16) | (g << 8) | b;

			// Set pixel color
			int fb_index = (y * finfo.line_length) + (x * bytes_per_pixel);
			memcpy(fb_data + fb_index, &blended_pixel, bytes_per_pixel);
		}
	}
}

int progress = 0;
uint8_t *fb_data;
struct fb_fix_screeninfo finfo;
struct fb_var_screeninfo vinfo;
int bar_width, bar_height;
size_t fb_size;
int fb_fd;
uint8_t *image_data;

void cleanup_and_exit() {
	munmap(fb_data, fb_size);
	close(fb_fd);
	free(image_data);
	exit(EXIT_SUCCESS);
}

void handle_sigusr1(int sig) {
	if (progress < 100) {
		progress += 10;
		draw_progress_bar(fb_data, finfo, vinfo, bar_width, bar_height, progress);
	}
	if (progress >= 100) {
		cleanup_and_exit();
	}
}

int main(int argc, char *argv[]) {
	if (argc < 5) {
		fprintf(stderr, "Usage: %s <file.ppm> <bar_width> <bar_height> <step_count>\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *ppm_file = argv[1];
	bar_width = atoi(argv[2]);
	bar_height = atoi(argv[3]);
	int step_count = atoi(argv[4]);
	const char *fb_device = "/dev/fb0";

	FILE *fp = fopen(ppm_file, "rb");
	if (!fp) {
		perror("Failed to open PPM file");
		return EXIT_FAILURE;
	}

	PPMHeader header;
	if (read_ppm_header(fp, &header) < 0) {
		fclose(fp);
		return EXIT_FAILURE;
	}

	size_t image_size = header.width * header.height * 3;
	image_data = malloc(image_size);
	if (fread(image_data, 1, image_size, fp) != image_size) {
		fprintf(stderr, "Failed to read PPM image data\n");
		free(image_data);
		fclose(fp);
		return EXIT_FAILURE;
	}
	fclose(fp);

	fb_fd = open(fb_device, O_RDWR);
	if (fb_fd < 0) {
		perror("Failed to open framebuffer device");
		free(image_data);
		return EXIT_FAILURE;
	}

	ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

	fb_size = finfo.line_length * vinfo.yres;
	fb_data = mmap(0, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

	int screen_width = vinfo.xres;
	int screen_height = vinfo.yres;
	int bytes_per_pixel = vinfo.bits_per_pixel / 8;

	int start_x = (screen_width - header.width) / 2;
	int start_y = (screen_height - header.height) / 2;

	for (int y = 0; y < header.height && (start_y + y) < screen_height; y++) {
		for (int x = 0; x < header.width && (start_x + x) < screen_width; x++) {
			int ppm_index = (y * header.width + x) * 3;
			int fb_index = ((start_y + y) * finfo.line_length) + ((start_x + x) * bytes_per_pixel);

			uint8_t r = image_data[ppm_index];
			uint8_t g = image_data[ppm_index + 1];
			uint8_t b = image_data[ppm_index + 2];

			uint32_t pixel;
			if (vinfo.bits_per_pixel == 32) {
				pixel = (r << 16) | (g << 8) | b;
			}
			else if (vinfo.bits_per_pixel == 16) {
				pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			}
			else {
				fprintf(stderr, "Unsupported framebuffer format: %d bpp\n", vinfo.bits_per_pixel);
				munmap(fb_data, fb_size);
				close(fb_fd);
				free(image_data);
				return EXIT_FAILURE;
			}
			memcpy(fb_data + fb_index, &pixel, bytes_per_pixel);
		}
	}

	draw_progress_bar(fb_data, finfo, vinfo, bar_width, bar_height, progress);

	signal(SIGUSR1, handle_sigusr1);

	while (1)
		pause();

	cleanup_and_exit();

	return EXIT_SUCCESS;
}
