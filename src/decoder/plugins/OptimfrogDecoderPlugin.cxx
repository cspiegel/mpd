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

#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

#include <OptimFROG/OptimFROG.h>

#include "config.h"
#include "Log.hxx"
#include "decoder/DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "tag/TagHandler.hxx"
#include "util/Domain.hxx"
#include "util/Error.hxx"

#include "OptimfrogDecoderPlugin.hxx"

static constexpr Domain optimfrog_domain("optimfrog");

class OFR {
public:
	explicit OFR(InputStream &file) : decoder(OptimFROG_createInstance()) {
		static ReadInterface rint = {
			ofr_close,
			ofr_read,
			ofr_eof,
			ofr_seekable,
			ofr_length,
			ofr_get_pos,
			ofr_seek,
		};

		if (decoder == C_NULL) {
			throw std::runtime_error("cannot create decoder instance");
		}

		if (!OptimFROG_openExt(decoder, &rint, &file, C_FALSE)) {
			OptimFROG_destroyInstance(decoder);
			throw std::runtime_error("cannot open file");
		}

		OptimFROG_getInfo(decoder, &info);

		/* 24- and 32-bit audio is converted to 16-bit. */
		if (info.bitspersample > 16) info.bitspersample = 16;
		if (std::strncmp(info.sampleType, "SINT", 4) != 0 && std::strncmp(info.sampleType, "UINT", 4) != 0) {
			OptimFROG_destroyInstance(decoder);
			throw std::runtime_error("invalid sample type");
		}

		is_signed = info.sampleType[0] == 'S';
	}

	OFR(const OFR &) = delete;
	OFR &operator=(const OFR &) = delete;

	~OFR() {
		OptimFROG_close(decoder);
		OptimFROG_destroyInstance(decoder);
	}

	sInt32_t read(std::vector<std::uint8_t> &buffer) {
		sInt32_t n;
		int bytes = depth() / 8;
		sInt32_t point_conversion = bytes * info.channels;

		n = OptimFROG_read(decoder, &buffer[0], buffer.size() / point_conversion, C_TRUE);

		n = n > 0 ? n * point_conversion : 0;

		/* mpd doesn't support unsigned samples, so convert here. */
		if (!is_signed) {
			unsigned char *overlay = reinterpret_cast<unsigned char *>(&buffer[0]);

			for (sInt32_t i = bytes - 1; i < n; i += bytes) {
				overlay[i] ^= 0x80;
			}
		}

		return n;
	}

	void seek(int pos) {
		if (OptimFROG_seekable(decoder)) {
			OptimFROG_seekTime(decoder, pos);
		}
	}

	SampleFormat format() { return depth() == 16 ? SampleFormat::S16 : SampleFormat::S8; }
	long rate() { return info.samplerate; }
	long channels() { return info.channels; }
	long depth() { return info.bitspersample; }
	long duration() { return info.length_ms; }

private:
	void *decoder;
	OptimFROG_Info info;

	bool is_signed;

	static InputStream *VFS(void *instance) { return reinterpret_cast<InputStream *>(instance); }

	static condition_t ofr_close(void *) { return C_TRUE; }
	static sInt32_t ofr_read(void *instance, void *buf, uInt32_t n) { Error e; return VFS(instance)->LockRead(buf, n, e); }
	static condition_t ofr_eof(void* instance) { return VFS(instance)->LockIsEOF(); }
	static condition_t ofr_seekable(void* instance) { return VFS(instance)->IsSeekable(); }
	static sInt64_t ofr_length(void* instance) { return VFS(instance)->KnownSize() ? VFS(instance)->GetSize() : 0; }
	static sInt64_t ofr_get_pos(void* instance) { return VFS(instance)->GetOffset(); }
	static condition_t ofr_seek(void* instance, sInt64_t offset) { Error e; return VFS(instance)->LockSeek(offset, e); }
};

static void
optimfrog_decode(Decoder &decoder, InputStream &is)
{
	try {
		OFR ofr(is);
		AudioFormat audio_format(ofr.rate(), ofr.format(), ofr.channels());

		decoder_initialized(decoder, audio_format, is.IsSeekable(), SongTime::FromMS(ofr.duration()));

		while (true) {
			std::vector<std::uint8_t> buffer(16384);

			sInt32_t r = ofr.read(buffer);
			if (r == 0) {
				break;
			}

			DecoderCommand cmd = decoder_data(decoder, nullptr, &buffer[0], buffer.size(), 0);
			if (cmd == DecoderCommand::STOP) {
				break;
			} else if (cmd == DecoderCommand::SEEK) {
				ofr.seek(decoder_seek_time(decoder).ToMS());
				decoder_command_finished(decoder);
			}
		}
	} catch (const std::exception &e) {
		LogDebug(optimfrog_domain, e.what());
	}
}

static bool
optimfrog_scan_stream(InputStream &is, const struct tag_handler *handler, void *handler_ctx)
{
	try {
		OFR ofr(is);

		tag_handler_invoke_duration(handler, handler_ctx,
					    SongTime::FromMS(ofr.duration()));

		return true;
	} catch (const std::exception &e) {
		LogDebug(optimfrog_domain, e.what());
		return false;
	}
}

static const char *const optimfrog_suffixes[] = { "ofr", nullptr };

const struct DecoderPlugin optimfrog_decoder_plugin = {
	"optimfrog",
	nullptr,
	nullptr,
	optimfrog_decode,
	nullptr,
	nullptr,
	optimfrog_scan_stream,
	nullptr,
	optimfrog_suffixes,
	nullptr,
};
