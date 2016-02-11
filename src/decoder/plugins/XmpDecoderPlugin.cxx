/*
 * Copyright (C) 2016 Chris Spiegel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <vector>

#include <xmp.h>

#include "config.h"
#include "Log.hxx"
#include "decoder/DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "system/FatalError.hxx"
#include "tag/TagHandler.hxx"
#include "util/Domain.hxx"
#include "util/Error.hxx"

#include "XmpDecoderPlugin.hxx"

static constexpr Domain libxmp_domain("xmp");
static constexpr offset_type LIBXMP_FILE_LIMIT = 100 * 1024 * 1024;

static constexpr int DEFAULT_PANNING_AMPLITUDE = 50;
static constexpr int DEFAULT_STEREO_SEPARATION = 70;

static int panning_amplitude = DEFAULT_PANNING_AMPLITUDE;
static int stereo_separation = DEFAULT_STEREO_SEPARATION;

class Xmp {
public:
	struct Frame {
		Frame(int n_, void *buf_) : n(n_), buf(buf_) { }
		int n;
		void *buf;
	};

	Xmp(Decoder *decoder, InputStream &is) : ctx(xmp_create_context()) {
		if (ctx == nullptr) {
			throw std::runtime_error("cannot create xmp context");
		}

		xmp_set_player(ctx, XMP_PLAYER_DEFPAN, panning_amplitude);

		auto buffer = load_module(decoder, is);
		if (xmp_load_module_from_memory(ctx, &buffer[0], buffer.size()) != 0) {
			xmp_free_context(ctx);
			throw std::runtime_error("cannot load module");
		}

		if (xmp_start_player(ctx, rate(), 0) != 0) {
			xmp_release_module(ctx);
			xmp_free_context(ctx);
			throw std::runtime_error("cannot start playing module");
		}

		xmp_set_player(ctx, XMP_PLAYER_MIX, stereo_separation);
		xmp_get_module_info(ctx, &module_info);
	}

	Xmp(const Xmp &) = delete;
	Xmp &operator=(const Xmp &) = delete;

	~Xmp() {
		xmp_end_player(ctx);
		xmp_release_module(ctx);
		xmp_free_context(ctx);
	}

	long rate() { return 44100; }
	long channels() { return 2; }
	SampleFormat format() { return SampleFormat::S16; }
	long duration() { return module_info.seq_data[0].duration; }
	const char *title() { return module_info.mod->name; }
	const char *comment() { return module_info.comment; };

	Frame play_frame() {
		struct xmp_frame_info fi;

		if (xmp_play_frame(ctx) != 0) {
			return Frame(0, nullptr);
		}

		xmp_get_frame_info(ctx, &fi);
		if (fi.loop_count > 0) {
			return Frame(0, nullptr);
		}

		return Frame(fi.buffer_size, fi.buffer);
	}

	void seek(int pos) {
		struct xmp_frame_info fi[2];

		xmp_get_frame_info(ctx, &fi[0]);
		xmp_seek_time(ctx, pos);
		xmp_get_frame_info(ctx, &fi[1]);

		/* XMP seeks on a pattern-by-pattern basis,
		 * approximating the requested seek time.  If the
		 * pattern is so long that the seek time would stay on
		 * the same pattern, jump to the next pattern.  Make an
		 * exception, though, if the desired seek time is zero.
		 */
		if (pos > 0 && fi[0].pos == fi[1].pos) xmp_set_position(ctx, fi[1].pos + 1);
	}

private:
	xmp_context ctx;
	struct xmp_module_info module_info;

	static std::vector<std::uint8_t> load_module(Decoder *decoder, InputStream &is) {
		std::vector<std::uint8_t> buffer;

		while (true) {
			size_t ret;
			std::vector<std::uint8_t> chunk(8192);

			ret = decoder_read(decoder, is, &chunk[0], chunk.size());
			if (ret == 0) {
				if (is.LockIsEOF()) {
					break;
				} else {
					throw std::runtime_error("i/o error while reading file");
				}
			}

			chunk.resize(ret);
			buffer.insert(buffer.end(), chunk.begin(), chunk.end());

			if (buffer.size() > LIBXMP_FILE_LIMIT) {
				throw std::runtime_error("file is too large");
			}
		}

		return buffer;
	}
};

static bool
libxmp_init(const ConfigBlock &block)
{
	panning_amplitude = block.GetBlockValue("panning_amplitude", DEFAULT_PANNING_AMPLITUDE);
	if (panning_amplitude < 0 || panning_amplitude > 100) {
		FormatFatalError("invalid panning amplitude on line %d: is %d, must be in the range [0, 100]", block.line, panning_amplitude);
		return false;
	}

	stereo_separation = block.GetBlockValue("stereo_separation", DEFAULT_STEREO_SEPARATION);
	if (stereo_separation < 0 || stereo_separation > 100) {
		FormatFatalError("invalid stereo separation on line %d: is %d, must be in the range [0, 100]", block.line, stereo_separation);
		return false;
	}

	return true;
}

static void
libxmp_stream_decode(Decoder &decoder, InputStream &is)
{
	try {
		Xmp xmp(&decoder, is);
		AudioFormat audio_format(xmp.rate(), xmp.format(), xmp.channels());

		decoder_initialized(decoder, audio_format, is.IsSeekable(), SongTime::FromMS(xmp.duration()));

		while (true) {
			Xmp::Frame frame = xmp.play_frame();
			if (frame.n == 0) {
				break;
			}

			DecoderCommand cmd = decoder_data(decoder, nullptr, frame.buf, frame.n, 0);
			if (cmd == DecoderCommand::STOP) {
				break;
			} else if (cmd == DecoderCommand::SEEK) {
				xmp.seek(decoder_seek_time(decoder).ToMS());
				decoder_command_finished(decoder);
			}
		}
	} catch (const std::exception &e) {
		LogWarning(libxmp_domain, e.what());
		return;
	}
}

static bool
libxmp_scan_stream(InputStream &is, const struct tag_handler *handler, void *handler_ctx)
{
	try {
		Xmp xmp(nullptr, is);

		tag_handler_invoke_duration(handler, handler_ctx,
					    SongTime::FromMS(xmp.duration()));

		const char *title = xmp.title();
		if (title != nullptr) {
			tag_handler_invoke_tag(handler, handler_ctx,
					       TAG_TITLE, title);
		}

		const char *comment = xmp.comment();
		if (comment != nullptr) {
			tag_handler_invoke_tag(handler, handler_ctx,
					       TAG_COMMENT, comment);
		}

		return true;
	} catch (const std::exception &e) {
		return false;
	}
}

static const char *const libxmp_suffixes[] = {
	"669", "amf", "dbm", "digi", "emod", "far", "fnk",
	"gdm", "gmc", "imf", "ims", "it", "j2b", "liq", "mdl",
	"med", "mgt", "mod", "mtm", "ntp", "oct", "okta", "psm",
	"ptm", "rad", "rtm", "s3m", "stm", "ult", "umx", "xm",
	nullptr,
};

const struct DecoderPlugin xmp_decoder_plugin = {
	"xmp",			/* name */
	libxmp_init,		/* init */
	nullptr,		/* finish */
	libxmp_stream_decode,	/* stream_decode */
	nullptr,		/* file_decode */
	nullptr,		/* scan_file */
	libxmp_scan_stream,	/* scan_stream */
	nullptr,		/* container_scan */
	libxmp_suffixes,	/* suffixes */
	nullptr,		/* mime_types */
};
