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

#include <libopenmpt/libopenmpt.h>

#include "config.h"
#include "Log.hxx"
#include "decoder/DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "system/FatalError.hxx"
#include "tag/TagHandler.hxx"
#include "util/Domain.hxx"
#include "util/Error.hxx"

#include "OpenMPTDecoderPlugin.hxx"

static constexpr Domain openmpt_domain("openmpt");

static constexpr int DEFAULT_STEREO_SEPARATION = 100;

static int stereo_separation = DEFAULT_STEREO_SEPARATION;

class OpenMPT {
public:
	explicit OpenMPT(InputStream &file) {
		openmpt_stream_callbacks callbacks = { stream_read, stream_seek, stream_tell };
		mod = openmpt_module_create(callbacks, &file, openmpt_log_func_silent, nullptr, nullptr);
		if (mod == nullptr) {
			throw std::runtime_error("cannot create module context");
		}

		openmpt_module_select_subsong(mod, -1);
		openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, stereo_separation);

		duration_ = openmpt_module_get_duration_seconds(mod) * 1000;
		artist_ = copystr(openmpt_module_get_metadata(mod, "artist"));
		title_ = copystr(openmpt_module_get_metadata(mod, "title"));
		comment_ = copystr(openmpt_module_get_metadata(mod, "message_raw"));
	}

	~OpenMPT() {
		openmpt_module_destroy(mod);
	}

	SampleFormat format() { return SampleFormat::S16; }
	int rate() { return 44100; }
	int channels() { return 2; }
	int duration() { return duration_; }
	std::string artist() { return artist_; }
	std::string title() { return title_; }
	std::string comment() { return comment_; }

	int read(std::vector<std::uint8_t> &buffer) {
		std::size_t n;

		n = openmpt_module_read_interleaved_stereo(mod, rate(), buffer.size() / sizeof(std::int16_t) / channels(), reinterpret_cast<std::int16_t *>(&buffer[0]));

		return n * channels() * sizeof(std::int16_t);
	}

	void seek(int pos) {
		openmpt_module_set_position_seconds(mod, pos / 1000.0);
	}

private:
	std::string copystr(const char *src) {
		if (src != nullptr) {
			std::string dst = src;
			openmpt_free_string(src);
			return dst;
		} else {
			return "";
		}
	}

	static std::size_t stream_read(void *instance, void *buf, std::size_t n) {
		Error e;
		return VFS(instance)->LockRead(buf, n, e);
	}

	static int stream_seek(void *instance, std::int64_t offset, int whence) {
		offset_type pos;

		if (!VFS(instance)->IsSeekable()) {
			return -1;
		}

		switch(whence) {
			case OPENMPT_STREAM_SEEK_SET:
				pos = offset;
				break;
			case OPENMPT_STREAM_SEEK_CUR:
				pos = VFS(instance)->GetOffset() + offset;
				break;
			case OPENMPT_STREAM_SEEK_END:
				pos = VFS(instance)->GetSize() + offset;
				break;
			default:
				return -1;
		}

		Error e;
		return VFS(instance)->LockSeek(pos, e) ? 0 : -1;
	}

	static std::int64_t stream_tell(void *instance) {
		return VFS(instance)->GetOffset();
	}

	static InputStream *VFS(void *instance) { return reinterpret_cast<InputStream *>(instance); }

	openmpt_module *mod;
	int duration_;
	std::string artist_;
	std::string title_;
	std::string comment_;
};

static bool
openmpt_init(const ConfigBlock &block)
{
	stereo_separation = block.GetBlockValue("stereo_separation", DEFAULT_STEREO_SEPARATION);
	if (stereo_separation < 0 || stereo_separation > 200) {
		FormatFatalError("invalid stereo separation on line %d: is %d, must be in the range [0, 200]", block.line, stereo_separation);
		return false;
	}

	return true;
}

static void
openmpt_stream_decode(Decoder &decoder, InputStream &is)
{
	try {
		OpenMPT mpt(is);
		AudioFormat audio_format(mpt.rate(), mpt.format(), mpt.channels());

		decoder_initialized(decoder, audio_format, is.IsSeekable(), SongTime::FromMS(mpt.duration()));

		while (true) {
			int r;
			std::vector<std::uint8_t> buffer(16384);

			r = mpt.read(buffer);
			if (r == 0) {
				break;
			}

			DecoderCommand cmd = decoder_data(decoder, nullptr, &buffer[0], r, 0);
			if (cmd == DecoderCommand::STOP) {
				break;
			} else if (cmd == DecoderCommand::SEEK) {
				mpt.seek(decoder_seek_time(decoder).ToMS());
				decoder_command_finished(decoder);
			}
		}
	} catch (const std::exception &e) {
		LogWarning(openmpt_domain, e.what());
		return;
	}
}

static bool
openmpt_scan_stream(InputStream &is, const struct tag_handler *handler, void *handler_ctx)
{
	try {
		OpenMPT mpt(is);

		tag_handler_invoke_duration(handler, handler_ctx,
					    SongTime::FromMS(mpt.duration()));

		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_ARTIST, mpt.artist().c_str());

		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_TITLE, mpt.title().c_str());

		tag_handler_invoke_tag(handler, handler_ctx,
				       TAG_COMMENT, mpt.comment().c_str());

		return true;
	} catch (const std::exception &e) {
		return false;
	}
}

static const char *const openmpt_suffixes[] = {
	"669", "amf", "dbm", "digi", "emod", "far", "fnk",
	"gdm", "gmc", "imf", "ims", "it", "j2b", "liq", "mdl",
	"med", "mgt", "mod", "mtm", "ntp", "oct", "okta", "psm",
	"ptm", "rad", "rtm", "s3m", "stm", "ult", "umx", "xm",
	nullptr,
};

const struct DecoderPlugin openmpt_decoder_plugin = {
	"openmpt",		/* name */
	openmpt_init,		/* init */
	nullptr,		/* finish */
	openmpt_stream_decode,	/* stream_decode */
	nullptr,		/* file_decode */
	nullptr,		/* scan_file */
	openmpt_scan_stream,	/* scan_stream */
	nullptr,		/* container_scan */
	openmpt_suffixes,	/* suffixes */
	nullptr,		/* mime_types */
};
