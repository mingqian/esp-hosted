/* Linux boot Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiny_jffs2_reader.h"
#include "bootparam.h"

static const esp_partition_t *find_partition(const char *name)
{
	esp_partition_iterator_t it;
	const esp_partition_t *part;

	it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
	if (!it)
		return NULL;
	part = esp_partition_get(it);
	return part;
}

static void check_partition_mapping(const char *name, const esp_partition_t *part,
				    const void *ptr)
{
	const uint32_t mask = 0x01ffffff;

	if (((uint32_t)ptr & mask) != (part->address & mask)) {
		ESP_LOGE(__func__, "mapping %s: expected: 0x%08x, actual: %p\n",
			 name, part->address & mask, ptr);
		abort();
	}
}

static const void *map_partition_part(const esp_partition_t *part, uint32_t size)
{
	const void *ptr;
	spi_flash_mmap_handle_t handle;

	if (esp_partition_mmap(part, 0, size,
			       SPI_FLASH_MMAP_INST, &ptr, &handle) != ESP_OK)
		abort();
	return ptr;
}

static const void *map_partition_name_part(const char *name, uint32_t size)
{
	const esp_partition_t *part = find_partition(name);

	if (!part)
		return NULL;
	return map_partition_part(part, size);
}

static void align_mapping_address(uint32_t addr)
{
	const esp_partition_t *part = find_partition("factory");
	uint32_t sz = 0x10000;

	for (;;) {
		const void *ptr = map_partition_part(part, sz);
		uint32_t next_map = ((uint32_t)ptr & 0x01ffffff) + sz;

		if (next_map == addr)
			return;
		if (!ptr || next_map > addr)
			abort();
		sz += addr - next_map;
	}
}

#define CMDLINE_MAX 260

#ifdef CONFIG_LINUX_COMMAND_LINE
static void parse_cmdline(const void *ptr, uint32_t size, struct bp_tag tag[])
{
	struct jffs2_image img = {
		.data = (void *)ptr,
		.sz = size,
	};
	uint32_t cmdline_inode = jffs2_lookup(&img, 1, "cmdline");

	if (cmdline_inode) {
		char *cmdline = (char *)tag[1].data;
		size_t rd = jffs2_read(&img, cmdline_inode, cmdline, CMDLINE_MAX - 1);

		if (rd != -1) {
			tag[1].id = BP_TAG_COMMAND_LINE;
			cmdline[rd] = 0;
			ESP_LOGI(__func__, "found /etc/cmdline [%d] = '%s'\n", rd, cmdline);
		}
	}
}
#endif

static const void *map_partition_range(uint32_t start, uint32_t end, struct bp_tag tag[])
{
	const void *rv = NULL;
	esp_partition_iterator_t it;

	it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
	while (it) {
		const esp_partition_t *part = esp_partition_get(it);

		if (part->address > start)
			align_mapping_address(part->address);

		if (part->address >= start &&
		    part->address + part->size <= end) {
			const void *ptr = map_partition_part(part, part->size);

			check_partition_mapping(part->label, part, ptr);
			ESP_LOGD(__func__, "0x%08x/0x%08x -> %p \"%s\"",
				 part->address, part->size, ptr, part->label);
			start = part->address + part->size;
			if (!strcmp(part->label, "linux"))
				rv = ptr;
#ifdef CONFIG_LINUX_COMMAND_LINE
			if (!strcmp(part->label, "etc"))
				parse_cmdline(ptr, part->size, tag);
#endif
		}
		it = esp_partition_next(it);
	}
	return rv;
}

static void map_psram_to_iram(void)
{
	uint32_t *dst = (uint32_t *)DR_REG_MMU_TABLE + 0x100;
	uint32_t *src = (uint32_t *)DR_REG_MMU_TABLE + 0x180;
	int i;

	for (i = 0; i < 0x80; ++i) {
		dst[i] = src[i];
	}
}

static void cache_partition(const char *name)
{
	esp_partition_iterator_t it;
	const esp_partition_t *part;
	char v;

	it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
	part = esp_partition_get(it);
	if (esp_partition_read(part, 0, &v, 1) != ESP_OK)
		abort();
}

static char IRAM_ATTR space_for_vectors[4096] __attribute__((aligned(4096)));

#define N_TAGS (3 + CMDLINE_MAX / sizeof(struct bp_tag))

static void map_flash_and_go(void)
{
	struct bp_tag tag[N_TAGS] = {
		[0] = {.id = BP_TAG_FIRST},
		[1] = {.id = BP_TAG_LAST, .size = CMDLINE_MAX},
		[N_TAGS - 1] = {.id = BP_TAG_LAST},
	};
	const void *ptr = map_partition_name_part("factory", 0x10000);
	uint32_t start = ((uint32_t)ptr & 0x01ffffff) + 0x10000;
	uint32_t end = 0x01000000;

	ptr = map_partition_range(start, end, tag);
	printf("linux ptr = %p\n", ptr);
	printf("vectors ptr = %p\n", space_for_vectors);

	map_psram_to_iram();

	cache_partition("nvs");

	extern int g_abort_on_ipc;
	g_abort_on_ipc = 1;

	asm volatile ("mov a2, %1 ; jx %0" :: "r"(ptr), "r"(tag) : "a2");
}

static void linux_task(void *p)
{
	map_flash_and_go();
	esp_restart();
}

void linux_boot(void)
{
	xTaskCreatePinnedToCore(linux_task, "linux_task", 4096, NULL, 5, NULL, 1);
}
