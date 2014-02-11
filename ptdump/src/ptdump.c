/*
 * Copyright (c) 2013-2014, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "pt_print.h"

#include "pt_cpu.h"
#include "pt_last_ip.h"

#include "intel-pt.h"

#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#if defined(FEATURE_MMAP)
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/mman.h>
# include <fcntl.h>
# include <unistd.h>
#endif


enum pt_dump_flag {
	/* Show the current offset in the trace stream. */
	ptd_show_offset = 1 << 0,

	/* Show raw packet bytes. */
	ptd_show_raw_bytes = 1 << 1,

	/* Show last IP for packets with IP payloads. */
	ptd_show_last_ip = 1 << 2,

	/* Print current offset column always with fixed width. */
	ptd_fixed_offset_width = 1 << 3,

	/* Use the cpu specified in the options. */
	ptd_use_cpu = 1 << 4,

	/* Quiet mode: Don't print anything but errors. */
	ptd_quiet = 1 << 5,

	/* Don't show PAD packets. */
	ptd_no_pad = 1 << 6
};

struct ptdump_options {
	uint32_t flags;
	struct pt_cpu cpu;
};

static int usage(const char *name)
{
	fprintf(stderr,
		"%s: [<options>] <ptfile>.  Use --help or -h for help.\n",
		name);
	return -1;
}

static int no_file_error(const char *name)
{
	fprintf(stderr, "%s: No processor trace file specified.\n", name);
	return -1;
}

static int help(const char *name)
{
	fprintf(stderr,
		"usage: %s [<options>] <ptfile>\n\n"
		"options:\n"
		"  --help|-h                this text.\n"
		"  --version                display version information and exit.\n"
		"  --quiet                  don't print anything but errors.\n"
		"  --no-pad                 don't show PAD packets.\n"
		"  --no-offset              don't show the offset as the first column.\n"
		"  --raw                    show raw packet bytes.\n"
		"  --lastip                 show last IP updates on packets with IP payloads.\n"
		"  --fixed-offset-width     assume fixed width of 16 characters for the\n"
		"                           offset column.\n"
		"  --cpu none|auto|f/m[/s]  set cpu to the given value and decode according to:\n"
		"                             none     spec (default)\n"
		"                             auto     current cpu\n"
		"                             f/m[/s]  family/model[/stepping]\n",
		name);

	return 0;
}

static int version(const char *name)
{
	struct pt_version v = pt_library_version();

	printf("%s-%d.%d.%d%s / libipt-%" PRIu8 ".%" PRIu8 ".%" PRIu32 "%s\n",
	       name, PT_VERSION_MAJOR, PT_VERSION_MINOR, PT_VERSION_BUILD,
	       PT_VERSION_EXT, v.major, v.minor, v.build, v.ext);
	return 0;
}

#if defined(FEATURE_MMAP)

static void *map_pt(const char *file, uint32_t *size)
{
	struct stat stat;
	int fd, errcode;
	void *pt;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s: %s\n", file,
			strerror(errno));
		return NULL;
	}

	errcode = fstat(fd, &stat);
	if (errcode) {
		fprintf(stderr, "failed to fstat %s: %s\n", file,
			strerror(errno));
		close(fd);
		return NULL;
	}

	pt = mmap(NULL, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (pt == MAP_FAILED) {
		fprintf(stderr, "failed to mmap %s: %s\n", file,
			strerror(errno));
		close(fd);
		return NULL;
	}

	if (size)
		*size = stat.st_size;

	close(fd);
	return pt;
}

#else /* defined(FEATURE_MMAP) */

static void *map_pt(const char *file, uint32_t *size)
{
	FILE *fd;
	uint32_t fsize;
	int errcode;
	long pos;
	void *pt;
	size_t read;

	errno = 0;
	fd = fopen(file, "rb");
	if (!fd) {
		fprintf(stderr, "%s: cannot open: %s\n", file, strerror(errno));
		return NULL;
	}

	errcode = fseek(fd, 0, SEEK_END);
	if (errcode) {
		fprintf(stderr, "%s: failed to seek end: %s\n", file,
			strerror(errno));
		fclose(fd);
		return NULL;
	}

	pos = ftell(fd);
	if (pos < 0) {
		fprintf(stderr, "%s: failed to determine file size: %s\n",
			file, strerror(errno));
		fclose(fd);
		return NULL;
	}
	if (pos == 0) {
		fprintf(stderr, "%s: file empty\n", file);
		fclose(fd);
		return NULL;
	}
	fsize = (size_t) pos;

	errcode = fseek(fd, 0, SEEK_SET);
	if (errcode) {
		fprintf(stderr, "%s: failed to seek begin: %s\n", file,
			strerror(errno));
		fclose(fd);
		return NULL;
	}

	pt = malloc(fsize);
	if (!pt) {
		fprintf(stderr,
			"%s: failed to allocate %" PRIu32 " bytes: %s\n",
			file, fsize, strerror(errno));
		fclose(fd);
		return NULL;
	}

	read = fread(pt, 1, fsize, fd);

	errcode = ferror(fd);
	if (errcode) {
		fprintf(stderr, "%s: failed to read file: %s\n", file,
			strerror(errcode));
		fclose(fd);
		free(pt);
		return NULL;
	}

	if (size)
		*size = (uint32_t) read;

	fclose(fd);
	return pt;
}

#endif /* defined(FEATURE_MMAP) */

static int print(const struct ptdump_options *options, const char *format, ...)
{
	int ret;
	va_list ap;

	if (options->flags & ptd_quiet)
		return 0;

	va_start(ap, format);
	ret = vprintf(format, ap);
	va_end(ap);

	if (ret <= 0)
		return -pte_internal;

	return ret;
}

static inline unsigned calc_col_offset_width(uint64_t highest_val)
{
	int idx = 63;
	for (idx = 63; idx > 0; --idx)
		if (highest_val & (1ull << idx))
			break;
	return 1 + (idx / 4);
}

static inline void print_col_separator(const struct ptdump_options *options)
{
	print(options, "  ");
}

static inline void fillup_column(const struct ptdump_options *options,
				 unsigned actual_width, unsigned col_width)
{
	if (actual_width >= col_width)
		return;
	print(options, "%*c", col_width - actual_width, ' ');
}

static inline void diag(const char *msg)
{
	fprintf(stderr, "[error: %s]\n", msg);
}

static inline void diag_pos(const char *msg, uint64_t pos)
{
	fprintf(stderr, "[%" PRIx64 ": error: %s]\n", pos, msg);
}

static inline void diag_err(const char *msg, enum pt_error_code err)
{
	fprintf(stderr, "[error: %s (%s)]\n", msg, pt_errstr(err));
}

static inline void diag_err_pos(const char *msg, enum pt_error_code err,
				uint64_t pos)
{
	fprintf(stderr, "[%" PRIx64 ": error: %s (%s)]\n",
		pos, msg, pt_errstr(err));
}

static int dump(uint8_t *begin, uint8_t *end,
		const struct ptdump_options *options)
{
	int errcode;
	struct pt_packet packet;
	struct pt_config config;
	struct pt_packet_decoder *decoder;
	struct pt_last_ip last_ip;
	char str[pps_payload];
	int ret;
	uint64_t pos;

	unsigned col_offset_width_used = 0;
	unsigned col_packettype_width_used = 0;
	unsigned col_payload_width_used = 0;

	const unsigned col_offset_width_fixed = 16;
	const unsigned col_offset_width =
		(options->flags & ptd_fixed_offset_width) ?
		col_offset_width_fixed :
		calc_col_offset_width(end - begin);
	const unsigned col_packettype_width = 9;
	const unsigned col_payload_width = 47;

	errcode = 0;

	memset(&packet, 0, sizeof(packet));

	memset(&config, 0, sizeof(config));
	pt_configure(&config);
	config.begin = begin;
	config.end = end;

	/* check if we need to override the auto-detected value. */
	if (options->flags & ptd_use_cpu)
		config.cpu = options->cpu;

	decoder = NULL;
	decoder = pt_pkt_alloc_decoder(&config);
	if (!decoder) {
		diag("cannot allocate decoder");
		errcode = -pte_nomem;
		goto out;
	}


sync:
	/* Sync to the stream. */
	errcode = pt_pkt_sync_forward(decoder);
	if (errcode < 0) {
		uint64_t offset;
		int errcode2;

		errcode2 = pt_pkt_get_offset(decoder, &offset);
		if (errcode2 < 0) {
			diag_err("sync error", pt_errcode(errcode));
			diag_err("could not determine offset",
				 pt_errcode(errcode2));
		} else
			diag_err_pos("sync error", pt_errcode(errcode), offset);

		goto out;
	}

	pt_last_ip_init(&last_ip);


	for (;;) {
		/* Decode packet. */
		errcode = pt_pkt_get_offset(decoder, &pos);
		if (errcode < 0) {
			diag_err("determining offset failed",
				 pt_errcode(errcode));
			goto out;
		}

		ret = pt_pkt_next(decoder, &packet);
		if (pt_errcode(ret) == pte_eos)
			goto out;
		else if (pt_errcode(ret) != pte_ok) {
			diag_err_pos("packet decoding failed",
				     pt_errcode(ret), pos);
			errcode = ret;
			goto sync;
		}
		if (packet.size == 0) {
			diag_pos("packet decoding failed, "
				 "packet size is reported to be 0",
				 pos);
			errcode = -pte_bad_packet;
			goto sync;
		}

		/* Skip PAD packets if requested */
		if (packet.type == ppt_pad && (options->flags & ptd_no_pad))
			continue;

		/* Print stream offset. */
		if (options->flags & ptd_show_offset) {
			ret = print(options, "%0*" PRIx64, col_offset_width,
				    pos);
			if (ret < 0) {
				diag_pos("cannot print offset", pos);
				errcode = -pte_internal;
				goto sync;
			}
			col_offset_width_used = ret;

			fillup_column(options,
				col_offset_width_used, col_offset_width);
			print_col_separator(options);
		}

		/* Print packet type. */
		ret = print(options, "%s", pt_print_packet_type_str(&packet));
		if (ret < 0) {
			diag_pos("cannot print packet type", pos);
			errcode = -pte_internal;
			goto sync;
		}
		col_packettype_width_used = ret;

		/* Print the packet payload. */
		ret = pt_print_fill_payload_str(str, sizeof(str), &packet);
		if (ret < 0) {
			diag_pos("cannot print packet payload", pos);
			errcode = -pte_internal;
			goto sync;
		}
		if (ret > 0) {
			fillup_column(options,
				col_packettype_width_used,
				col_packettype_width);
			print_col_separator(options);
		}
		col_payload_width_used = print(options, "%s", str);

		/* Print last IP if requested
		 * and if packet type is an IP packet. */
		if (options->flags & ptd_show_last_ip) {
			uint64_t ip;

			switch (packet.type) {
			case ppt_tip:
			case ppt_tip_pge:
			case ppt_tip_pgd:
			case ppt_fup:
				ret = pt_last_ip_update_ip(&last_ip,
							   &packet.payload.ip,
							   &config);

				if (ret == -pte_invalid) {
					diag_err_pos("failed to update last-IP",
						     pte_invalid, pos);
					errcode = -pte_internal;
					goto sync;
				}
				if (ret == -pte_bad_packet) {
					diag_err_pos("failed to update last-IP",
						     pte_bad_packet, pos);
					errcode = -pte_bad_packet;
					goto sync;
				}
				if (ret == -pte_noip)
					goto skip_last_ip_printing;

				break;
			default:
				goto skip_last_ip_printing;
				break;
			}

			ret = pt_last_ip_query(&ip, &last_ip);
			if (ret == -pte_invalid) {
				diag_err_pos("cannot query last-IP",
					     pte_invalid, pos);
				errcode = -pte_internal;
				goto sync;
			}
			if (ret == -pte_noip)
				goto skip_last_ip_printing;
			if (ret == -pte_ip_suppressed)
				ret = print(options, ", ip=<suppressed>");
			else if (ret == 0)
				ret = print(options, ", ip=0x%016" PRIx64, ip);
			if (ret < 0) {
				diag_pos("cannot print last-IP", pos);
				errcode = -pte_internal;
				goto sync;
			}
			col_payload_width_used += ret;
		}
skip_last_ip_printing:

		/* Print raw packet bytes. */
		if (options->flags & ptd_show_raw_bytes) {
			uint8_t idx;

			if (col_payload_width_used == 0) {
				fillup_column(options,
					col_packettype_width_used,
					col_packettype_width);
				print_col_separator(options);
			}

			fillup_column(options,
				col_payload_width_used, col_payload_width);
			print_col_separator(options);

			print(options, "[");
			for (idx = 0; idx < packet.size; ++idx) {
				uint8_t u;

				u = *(pt_pkt_get_pos(decoder) + idx);
				print(options, "%02x", (unsigned)u);
				if (idx != (packet.size - 1))
					print(options, " ");
			}
			print(options, "]");
		}

		/* End of information printing for this packet. */
		print(options, "\n");
	}

out:
	pt_pkt_free_decoder(decoder);
	return errcode;
}

int main(int argc, char *argv[])
{
	struct ptdump_options options;
	uint32_t size;
	uint8_t *pt;
	int errcode, idx;
	const char *ptfile;

	ptfile = NULL;
	memset(&options, 0, sizeof(options));
	options.flags |= ptd_show_offset;

	/* default is to override the auto-detected value during
	 * pt_configure with default spec behavior. options.cpu is
	 * zeroed above.
	 */
	options.flags |= ptd_use_cpu;


	for (idx = 1; idx < argc; ++idx) {
		if (strncmp(argv[idx], "-", 1) != 0) {
			ptfile = argv[idx];
			if (idx < (argc-1))
				return usage(argv[0]);
			break;
		}

		if (strcmp(argv[idx], "-h") == 0)
			return help(argv[0]);
		if (strcmp(argv[idx], "--help") == 0)
			return help(argv[0]);
		if (strcmp(argv[idx], "--version") == 0)
			return version(argv[0]);
		else if (strcmp(argv[idx], "--quiet") == 0)
			options.flags |= ptd_quiet;
		else if (strcmp(argv[idx], "--no-pad") == 0)
			options.flags |= ptd_no_pad;
		else if (strcmp(argv[idx], "--no-offset") == 0)
			options.flags &= ~ptd_show_offset;
		else if (strcmp(argv[idx], "--raw") == 0)
			options.flags |= ptd_show_raw_bytes;
		else if (strcmp(argv[idx], "--lastip") == 0)
			options.flags |= ptd_show_last_ip;
		else if (strcmp(argv[idx], "--fixed-offset-width") == 0)
			options.flags |= ptd_fixed_offset_width;
		else if (strcmp(argv[idx], "--cpu") == 0) {
			const char *arg = argv[++idx];

			/* keep the auto-detected values from pt_configure. */
			if (strcmp(arg, "auto") == 0) {
				options.flags &= ~ptd_use_cpu;
				continue;
			}

			/* use the value in options.cpu, instead of
			 * auto-detected.
			 */
			options.flags |= ptd_use_cpu;

			if (strcmp(arg, "none") == 0) {
				memset(&options.cpu, 0, sizeof(options.cpu));
				continue;
			}

			errcode = pt_cpu_parse(&options.cpu, arg);
			if (errcode < 0) {
				fprintf(stderr,
					"%s: cpu must be specified as f/m[/s]\n",
					argv[0]);
				return 1;
			}
		}
		else
			return usage(argv[0]);
	}

	if (!ptfile)
		return no_file_error(argv[0]);

	/* We will leak the pt buffer. */
	pt = map_pt(ptfile, &size);
	if (!pt) {
		diag("failed to read PT stream");
		return -1;
	}

	errcode = dump(pt, pt + size, &options);

	return -errcode;
}
