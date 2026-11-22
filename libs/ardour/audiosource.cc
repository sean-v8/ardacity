/*
 * Copyright (C) 2000-2019 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2006-2014 David Robillard <d@drobilla.net>
 * Copyright (C) 2007-2016 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2008-2012 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2013-2015 John Emmas <john@creativepost.co.uk>
 * Copyright (C) 2015-2019 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2015 Nick Mainsbridge <mainsbridge@gmail.com>
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

#ifdef COMPILER_MSVC
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <float.h>
#include <cerrno>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <windows.h>

#else
#include <sys/mman.h>

#endif

#include <glib.h>
#include "pbd/gstdio_compat.h"

#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "pbd/file_utils.h"
#include "pbd/playback_buffer.h"
#include "pbd/scoped_file_descriptor.h"
#include "pbd/xml++.h"

#include "ardour/audiosource.h"
#include "ardour/rc_configuration.h"
#include "ardour/runtime_functions.h"
#include "ardour/session.h"
#include "ardour/utils.h"

#include "pbd/i18n.h"

#include "ardour/debug.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;

bool AudioSource::_build_missing_peakfiles = false;

/** true if we want peakfiles (e.g. if we are displaying a GUI) */
bool AudioSource::_build_peakfiles = false;

#define _FPP 256

AudioSource::AudioSource (Session& s, const string& name)
	: Source (s, DataType::AUDIO, name)
	, _peak_byte_max (0)
	, _peaks_built (false)
	, _peakfile_fd (-1)
	, peak_leftover_cnt (0)
	, peak_leftover_size (0)
	, peak_leftovers (0)
	, peak_leftover_sample (0)
	, _first_run (true)
	, _last_scale (0.0)
	, _last_map_off (0)
	, _last_raw_map_length (0)
{
}

AudioSource::AudioSource (Session& s, const XMLNode& node)
	: Source (s, node)
	, _peak_byte_max (0)
	, _peaks_built (false)
	, _peakfile_fd (-1)
	, peak_leftover_cnt (0)
	, peak_leftover_size (0)
	, peak_leftovers (0)
	, peak_leftover_sample (0)
	, _first_run (true)
	, _last_scale (0.0)
	, _last_map_off (0)
	, _last_raw_map_length (0)
{
	if (set_state (node, Stateful::loading_state_version)) {
		throw failed_constructor();
	}
}

AudioSource::~AudioSource ()
{
#ifndef NDEBUG
	/* shouldn't happen but make sure we don't leak file descriptors anyway,
	 * (it can happen with stop-and-forget capture)
	 */
	if (peak_leftover_cnt) {
		cerr << "AudioSource destroyed with leftover peak data pending" << endl;
	}
#endif

	if (-1 != _peakfile_fd) {
		close (_peakfile_fd);
		_peakfile_fd = -1;
	}

	delete [] peak_leftovers;
}

void
AudioSource::estimate_tempo ()
{
#ifndef NDEBUG
	/* CALLER MUST HOLD WRITER LOCK */
	assert (!_lock.writer_trylock());
#endif

	const samplecnt_t ten_seconds = _session.sample_rate() * 10;

	if (length().samples() < ten_seconds) {
		return;
	}

	samplecnt_t data_size = ten_seconds;
	std::unique_ptr<Sample[]> data (new Sample[data_size]);

	/* Read ten seconds from the middle of the data */

	if (read_unlocked (data.get(), std::max ((samplecnt_t) 0, (length().samples() - data_size) / 2), data_size) == data_size) {

		double tempo;
		double beatcount;
		Temporal::Tempo t (120, 4);
		Temporal::Meter m (4, 4);
		TimelineRange r (timepos_t (0), timepos_t (0) + length(), 0);

		estimate_audio_tempo_source (r, shared_from_this(), data.get(), data_size, _session.sample_rate(), tempo, m, beatcount);
		t = Temporal::Tempo (tempo, 4);

		SegmentDescriptor sd;

		sd.set_tempo (t);
		sd.set_meter (m);
		sd.set_position (0);
		sd.set_duration (length().samples());

		/* One segment descriptor to rule them all */

		set_segment_descriptor (sd, true);
	}
}

XMLNode&
AudioSource::get_state () const
{
	XMLNode& node (Source::get_state());

	if (_captured_for.length()) {
		node.set_property ("captured-for", _captured_for);
	}

	return node;
}

int
AudioSource::set_state (const XMLNode& node, int /*version*/)
{
	node.get_property ("captured-for", _captured_for);
	return 0;
}

void
AudioSource::update_length (timepos_t const & dur)
{
	assert (_length.time_domain() == dur.time_domain());

	/* audio files cannot get smaller via this mechanism */

	if (dur > _length) {
		_length = dur;
	}
}


/***********************************************************************
  PEAK FILE STUFF
 ***********************************************************************/

/** Checks to see if peaks are ready.  If so, we return true.  If not, we return false, and
 *  things are set up so that doThisWhenReady is called when the peaks are ready.
 *  A new PBD::ScopedConnection is created for the associated connection and written to
 *  *connect_here_if_not.
 *
 *  @param doThisWhenReady Function to call when peaks are ready (if they are not already).
 *  @param connect_here_if_not Address to write new ScopedConnection to.
 *  @param event_loop Event loop for doThisWhenReady to be called in.
 */
bool
AudioSource::peaks_ready (std::function<void()> doThisWhenReady, ScopedConnection** connect_here_if_not, EventLoop* event_loop) const
{
	bool ret;
	PBD::Mutex::Lock lm (_peaks_ready_lock);

	if (!(ret = _peaks_built)) {
		*connect_here_if_not = new ScopedConnection;
		PeaksReady.connect (**connect_here_if_not, MISSING_INVALIDATOR, doThisWhenReady, event_loop);
	}

	return ret;
}

void
AudioSource::touch_peakfile ()
{
	if (_flags & NoPeakFile) {
		return;
	}

	GStatBuf statbuf;

	if (g_stat (_peakpath.c_str(), &statbuf) != 0 || statbuf.st_size == 0) {
		return;
	}

	struct utimbuf tbuf;

	tbuf.actime = statbuf.st_atime;
	tbuf.modtime = time ((time_t*) 0);

	g_utime (_peakpath.c_str(), &tbuf);
}

int
AudioSource::rename_peakfile (string newpath)
{
	/* caller must hold _lock */

	string oldpath = _peakpath;

	if (Glib::file_test (oldpath, Glib::FILE_TEST_EXISTS)) {
		if (g_rename (oldpath.c_str(), newpath.c_str()) != 0) {
			error << string_compose (_("cannot rename peakfile for %1 from %2 to %3 (%4)"), _name, oldpath, newpath, strerror (errno)) << endmsg;
			return -1;
		}
	}

	_peakpath = newpath;

	return 0;
}

int
AudioSource::initialize_peakfile (const string& audio_path, const bool in_session)
{
	PBD::Mutex::Lock lm (_initialize_peaks_lock);
	GStatBuf statbuf;

	_peakpath = construct_peak_filepath (audio_path, in_session);

	if (!empty() && !Glib::file_test (_peakpath.c_str(), Glib::FILE_TEST_EXISTS)) {
		string oldpeak = construct_peak_filepath (audio_path, in_session, true);
		DEBUG_TRACE(DEBUG::Peaks, string_compose ("Looking for old peak file %1 for Audio file %2\n", oldpeak, audio_path));
		if (Glib::file_test (oldpeak.c_str(), Glib::FILE_TEST_EXISTS)) {
			// TODO use hard-link if possible
			DEBUG_TRACE(DEBUG::Peaks, string_compose ("Copy old peakfile %1 to %2\n", oldpeak, _peakpath));
			PBD::copy_file (oldpeak, _peakpath);
		}
	}

	DEBUG_TRACE(DEBUG::Peaks, string_compose ("Initialize Peakfile %1 for Audio file %2\n", _peakpath, audio_path));

	if (g_stat (_peakpath.c_str(), &statbuf)) {
		if (errno != ENOENT) {
			/* it exists in the peaks dir, but there is some kind of error */

			error << string_compose(_("AudioSource: cannot stat peakfile \"%1\""), _peakpath) << endmsg;
			return -1;
		}

		DEBUG_TRACE(DEBUG::Peaks, string_compose("Peakfile %1 does not exist\n", _peakpath));

		_peaks_built = false;

	} else {

		/* we found it in the peaks dir, so check it out */

		if (statbuf.st_size == 0 || (statbuf.st_size < (off_t) ((length().samples() / _FPP) * sizeof (PeakData)))) {
			DEBUG_TRACE(DEBUG::Peaks, string_compose("Peakfile %1 is empty\n", _peakpath));
			_peaks_built = false;
		} else {
			// Check if the audio file has changed since the peakfile was built.
			GStatBuf stat_file;
			int err = g_stat (audio_path.c_str(), &stat_file);

			if (err) {

				/* no audio path - nested source or we can't
				   read it or ... whatever, use the peakfile as-is.
				*/
				DEBUG_TRACE(DEBUG::Peaks, string_compose("Error when calling stat on Peakfile %1\n", _peakpath));

				_peaks_built = true;
				_peak_byte_max = statbuf.st_size;

			} else {

				/* allow 6 seconds slop on checking peak vs. file times because of various
				   disk action "races"
				*/

				if (stat_file.st_mtime > statbuf.st_mtime && (stat_file.st_mtime - statbuf.st_mtime > 6)) {
					_peaks_built = false;
					_peak_byte_max = 0;
				} else {
					_peaks_built = true;
					_peak_byte_max = statbuf.st_size;
				}
			}
		}
	}

	if (!empty() && !_peaks_built && _build_missing_peakfiles && _build_peakfiles) {
		build_peaks_from_scratch ();
	}

	return 0;
}

samplecnt_t
AudioSource::read (Sample *dst, samplepos_t start, samplecnt_t cnt, int /*channel*/) const
{
	assert (cnt >= 0);

	/* as odd as it may seem, given that this method is used to *read* the
	 * source, that we would need a write lock here. The problem is that
	 * the audio file API we use (libsndfile) does not allow concurrent use
	 * of the same SNDFILE object, even for reading. Consequently, even
	 * readers must be serialized.
	 */

	WriterLock lm (_lock);
	return read_unlocked (dst, start, cnt);
}

samplecnt_t
AudioSource::write (Sample const * src, samplecnt_t cnt)
{
	WriterLock lm (_lock);
	/* any write makes the file not removable */
	_flags = Flag (_flags & ~Removable);
	return write_unlocked (src, cnt);
}

static Sample
waveform_rms (const Sample* samples, samplecnt_t count)
{
	double energy = 0;
	for (samplecnt_t i = 0; i < count; ++i) {
		energy += double (samples[i]) * samples[i];
	}
	return count ? std::sqrt (energy / count) : 0;
}

int
AudioSource::read_peaks (PeakData *peaks, samplecnt_t npeaks, samplepos_t start, samplecnt_t cnt, double samples_per_visual_peak) const
{
	return read_peaks_with_fpp (peaks, npeaks, start, cnt, samples_per_visual_peak, _FPP);
}

/** @param peaks Buffer to write peak data.
 *  @param npeaks Number of peaks to write.
 */

int
AudioSource::read_peaks_with_fpp (PeakData *peaks, samplecnt_t npeaks, samplepos_t start, samplecnt_t cnt,
				  double samples_per_visual_peak, samplecnt_t samples_per_file_peak) const
{
	WriterLock lm (_lock);

#if 0 // DEBUG ONLY
	/* Bypass peak-file cache, compute peaks using raw data from source */
	DEBUG_TRACE (DEBUG::Peaks, string_compose ("RP: npeaks = %1 start = %2 cnt = %3 spp = %4 pf = %5\n", npeaks, start, cnt, samples_per_visual_peak, _peakpath));
	{
		samplecnt_t scm = ceil (samples_per_visual_peak);
		samplecnt_t peak = 0;

#if 1 // direct read
		std::unique_ptr<Sample[]> buf(new Sample[scm]);
		while (peak < npeaks && cnt > 0) {
			samplecnt_t samples_read = read_unlocked (buf.get(), start, scm);
			if (samples_read == 0) {
				break;
			}
			peaks[peak].min = peaks[peak].max = buf[0];
			peaks[peak].rms = waveform_rms (buf.get(), samples_read);
			find_peaks (buf.get(), samples_read, &peaks[peak].min, &peaks[peak].max);

			start += samples_read;
			cnt -= samples_read;
			++peak;
		}
#else // generate square wave / ramp
		while (peak < npeaks && cnt > 0) {
			samplecnt_t samples_read = std::min (cnt, scm);
			samplecnt_t val = (start + samples_read / 2) % 24000;

			peaks[peak].min = peaks[peak].max = .5 - val / 24000.0;
			peaks[peak].rms = std::abs (peaks[peak].max);

			start += samples_read;
			cnt -= samples_read;
			++peak;
		}
#endif
		while (peak < npeaks) {
			peaks[peak] = {};
			++peak;
		}
		return 0;
	}
#endif

	double scale;
	double expected_peaks;
#ifdef PLATFORM_WINDOWS
	SYSTEM_INFO system_info;
        GetSystemInfo (&system_info);
	const int bufsize = system_info.dwAllocationGranularity;;
#else
	const int bufsize = sysconf(_SC_PAGESIZE);
#endif
	samplecnt_t read_npeaks = npeaks;
	samplecnt_t zero_fill = 0;

	GStatBuf statbuf;

	expected_peaks = (cnt / (double) samples_per_file_peak);
	if (g_stat (_peakpath.c_str(), &statbuf) != 0) {
		error << string_compose (_("Cannot open peakfile @ %1 for size check (%2)"), _peakpath, strerror (errno)) << endmsg;
		return -1;
	}

	if (!_captured_for.empty()) {

		/* _captured_for is only set after a capture pass is
		 * complete. so we know that capturing is finished for this
		 * file, and now we can check actual size of the peakfile is at
		 * least large enough for all the data in the audio file. if it
		 * is too short, assume that a crash or other error truncated
		 * it, and rebuild it from scratch.
		 */

		const off_t expected_file_size = (_length.samples() / (double) samples_per_file_peak) * sizeof (PeakData);

		if (statbuf.st_size < expected_file_size) {
			warning << string_compose (_("peak file %1 is truncated from %2 to %3"), _peakpath, expected_file_size, statbuf.st_size) << endmsg;
			lm.release(); // build_peaks_from_scratch() takes _lock
			const_cast<AudioSource*>(this)->build_peaks_from_scratch ();
			lm.acquire ();
			if (g_stat (_peakpath.c_str(), &statbuf) != 0) {
				error << string_compose (_("Cannot open peakfile @ %1 for size check (%2) after rebuild"), _peakpath, strerror (errno)) << endmsg;
			}
			if (statbuf.st_size < expected_file_size) {
				fatal << "peak file is still truncated after rebuild" << endmsg;
				abort (); /*NOTREACHED*/
			}
		}
	}

	ScopedFileDescriptor sfd (g_open (_peakpath.c_str(), O_RDONLY, 0444));

	if (sfd < 0) {
		error << string_compose (_("Cannot open peakfile @ %1 for reading (%2)"), _peakpath, strerror (errno)) << endmsg;
		return -1;
	}

	scale = npeaks/expected_peaks;


	DEBUG_TRACE (DEBUG::Peaks, string_compose (" ======>RP: npeaks = %1 start = %2 cnt = %3 len = %4 samples_per_visual_peak = %5 expected was %6 ... scale =  %7 PD ptr = %8 pf = %9\n"
			, npeaks, start, cnt, _length.samples (), samples_per_visual_peak, expected_peaks, scale, peaks, _peakpath));

	/* fix for near-end-of-file conditions */

	if (cnt + start > _length.samples()) {
		// cerr << "too close to end @ " << _length << " given " << start << " + " << cnt << " (" << _length - start << ")" << endl;
		cnt = std::max ((samplecnt_t)0, _length.samples() - start);
		read_npeaks = min ((samplecnt_t) floor (cnt / samples_per_visual_peak), npeaks);
		zero_fill = npeaks - read_npeaks;
		expected_peaks = (cnt / (double) samples_per_file_peak);
		scale = npeaks/expected_peaks;
	}

	assert (cnt >= 0);

	// cerr << "actual npeaks = " << read_npeaks << " zf = " << zero_fill << endl;

	if (npeaks == cnt) {

		DEBUG_TRACE (DEBUG::Peaks, "RAW DATA\n");

		/* no scaling at all, just get the sample data and duplicate it for
		   both max and min peak values.
		*/

		std::unique_ptr<Sample[]> raw_staging(new Sample[cnt]);

		if (read_unlocked (raw_staging.get(), start, cnt) != cnt) {
			error << _("cannot read sample data for unscaled peak computation") << endmsg;
			return -1;
		}

		for (samplecnt_t i = 0; i < npeaks; ++i) {
			peaks[i].max = raw_staging[i];
			peaks[i].min = raw_staging[i];
			peaks[i].rms = std::abs (raw_staging[i]);
		}

		return 0;
	}

	if (scale == 1.0) {
		off_t first_peak_byte = (start / samples_per_file_peak) * sizeof (PeakData);
		size_t bytes_to_read = sizeof (PeakData) * read_npeaks;
		/* open, read, close */

		DEBUG_TRACE (DEBUG::Peaks, "DIRECT PEAKS\n");

		off_t  map_off =  first_peak_byte;
		off_t  read_map_off = map_off & ~(bufsize - 1);
		off_t  map_delta = map_off - read_map_off;
		size_t map_length = bytes_to_read + map_delta;

		if (_first_run  || (_last_scale != samples_per_visual_peak) || (_last_map_off != map_off) || (_last_raw_map_length  < bytes_to_read)) {
			peak_cache.reset (new PeakData[npeaks]);
			char* addr;
#ifdef PLATFORM_WINDOWS
			HANDLE file_handle = (HANDLE) _get_osfhandle(int(sfd));
			HANDLE map_handle;
			LPVOID view_handle;
			bool err_flag;

			map_handle = CreateFileMapping(file_handle, NULL, PAGE_READONLY, 0, 0, NULL);
			if (map_handle == NULL) {
				error << string_compose (_("map failed - could not create file mapping for peakfile %1."), _peakpath) << endmsg;
				return -1;
			}

			view_handle = MapViewOfFile(map_handle, FILE_MAP_READ, 0, read_map_off, map_length);
			if (view_handle == NULL) {
				error << string_compose (_("map failed - could not map peakfile %1."), _peakpath) << endmsg;
				return -1;
			}

			addr = (char*) view_handle;

			memcpy ((void*)peak_cache.get(), (void*)(addr + map_delta), bytes_to_read);

			err_flag = UnmapViewOfFile (view_handle);
			err_flag = CloseHandle(map_handle);
			if(!err_flag) {
				error << string_compose (_("unmap failed - could not unmap peakfile %1."), _peakpath) << endmsg;
				return -1;
			}
#else
			addr = (char*) mmap (0, map_length, PROT_READ, MAP_PRIVATE, sfd, read_map_off);
			if (addr ==  MAP_FAILED) {
				error << string_compose (_("map failed - could not mmap peakfile %1."), _peakpath) << endmsg;
				return -1;
			}

			memcpy ((void*)peak_cache.get(), (void*)(addr + map_delta), bytes_to_read);
			munmap (addr, map_length);
#endif
			if (zero_fill) {
				assert (read_npeaks < npeaks);
				memset (&peak_cache[read_npeaks], 0, sizeof (PeakData) * zero_fill);
			}

			_first_run = false;
			_last_scale = samples_per_visual_peak;
			_last_map_off = map_off;
			_last_raw_map_length = bytes_to_read;
		}

		memcpy ((void*)peaks, (void*)peak_cache.get(), npeaks * sizeof(PeakData));

		return 0;
	}

	if (scale < 1.0) {
		DEBUG_TRACE (DEBUG::Peaks, "DOWNSAMPLE\n");

		/* Assign stored peaks to columns relative to this image, including
		 * the first and last partial blocks. Each stored block is read once.
		 */
		std::fill_n (peaks, npeaks, PeakData {});
		if (cnt == 0) {
			return 0;
		}

		const samplepos_t first = start / samples_per_file_peak;
		const samplepos_t available = statbuf.st_size / sizeof (PeakData);
		const samplepos_t last = std::min (available, (start + cnt - 1) / samples_per_file_peak + 1);
		const off_t byte = first * sizeof (PeakData);
		if (first >= last || lseek (sfd, byte, SEEK_SET) != byte) {
			return -1;
		}

		std::vector<double> energy (npeaks, 0.0);
		std::vector<samplecnt_t> weight (npeaks, 0);
		PeakData blocks[4096];
		for (samplepos_t block = first; block < last;) {
			const samplecnt_t count = std::min<samplecnt_t> (4096, last - block);
			const size_t bytes = count * sizeof (PeakData);
			if (::read (sfd, blocks, bytes) != static_cast<ssize_t> (bytes)) {
				return -1;
			}
			for (samplecnt_t i = 0; i < count; ++i, ++block) {
				const samplepos_t block_start = block * samples_per_file_peak;
				const samplepos_t lo = std::max (start, block_start);
				const samplepos_t hi = std::min (start + cnt, block_start + samples_per_file_peak);
				const samplecnt_t first_column = std::min<samplecnt_t> (npeaks - 1, (lo - start) / samples_per_visual_peak);
				const samplecnt_t last_column = std::min<samplecnt_t> (npeaks - 1, (hi - 1 - start) / samples_per_visual_peak);
				for (samplecnt_t column = first_column; column <= last_column; ++column) {
					const samplepos_t left = start + std::ceil (column * samples_per_visual_peak);
					const samplepos_t right = start + std::ceil ((column + 1) * samples_per_visual_peak);
					const samplecnt_t overlap = std::min (hi, right) - std::max (lo, left);
					if (overlap <= 0) {
						continue;
					}
					PeakData& peak = peaks[column];
					peak.min = weight[column] ? std::min (peak.min, blocks[i].min) : blocks[i].min;
					peak.max = weight[column] ? std::max (peak.max, blocks[i].max) : blocks[i].max;
					energy[column] += double (blocks[i].rms) * blocks[i].rms * overlap;
					weight[column] += overlap;
				}
			}
		}
		for (samplecnt_t i = 0; i < npeaks; ++i) {
			peaks[i].rms = weight[i] ? std::sqrt (energy[i] / weight[i]) : 0;
		}

	} else {
		DEBUG_TRACE (DEBUG::Peaks, "UPSAMPLE\n");

		/* the caller wants
		 *
		 * - less samples-per-peak (more resolution)
		 * - more peaks than stored in the Peakfile
		 *
		 * So, fetch data from the raw source, and generate peak
		 * data on the fly.
		*/

		/* Use pixel boundaries relative to the image's first sample. Advancing
		 * from the previous sample's pixel can skip a column near 1 sample/pixel.
		 */
		if (cnt <= 0) {
			std::fill_n (peaks, npeaks, PeakData {});
			return 0;
		}
		const samplecnt_t chunk_size = std::min<samplecnt_t> (cnt, 4096);
		std::unique_ptr<Sample[]> buffer (new Sample[chunk_size]);
		samplecnt_t offset = 0;
		samplecnt_t in_buffer = 0;
		samplecnt_t index = 0;

		for (samplecnt_t column = 0; column < read_npeaks; ++column) {
			const samplecnt_t end = std::min<samplecnt_t> (cnt, std::llround ((column + 1) * samples_per_visual_peak));
			PeakData peak {};
			double energy = 0;
			const samplecnt_t count = std::max<samplecnt_t> (0, end - offset);
			for (samplecnt_t n = 0; n < count; ++n, ++offset) {
				if (index == in_buffer) {
					const samplecnt_t to_read = std::min (chunk_size, cnt - offset);
					in_buffer = read_unlocked (buffer.get (), start + offset, to_read);
					if (in_buffer <= 0) {
						return -1;
					}
					index = 0;
				}
				const Sample value = buffer[index++];
				peak.min = n ? std::min (peak.min, value) : value;
				peak.max = n ? std::max (peak.max, value) : value;
				energy += double (value) * value;
			}
			peak.rms = count ? std::sqrt (energy / count) : 0;
			peaks[column] = peak;
		}

		if (zero_fill) {
			memset (&peaks[read_npeaks], 0, sizeof (PeakData) * zero_fill);
		}
	}

	DEBUG_TRACE (DEBUG::Peaks, "READPEAKS DONE\n");
	return 0;
}

int
AudioSource::build_peaks_from_scratch ()
{
	constexpr samplecnt_t bufsize = 65536; // 256kB per disk read for mono data is about ideal

	DEBUG_TRACE (DEBUG::Peaks, "Building peaks from scratch\n");

	int ret = -1;

	{
		/* hold lock while building peaks */

		WriterLock lp (_lock);

		if (prepare_for_peakfile_writes ()) {
			goto out;
		}

		samplecnt_t current_sample = 0;
		samplecnt_t cnt = _length.samples();

		_peaks_built = false;
		std::unique_ptr<Sample[]> buf (new Sample[bufsize]);

		while (cnt) {

			samplecnt_t samples_to_read = min (bufsize, cnt);
			samplecnt_t samples_read;

			if ((samples_read = read_unlocked (buf.get(), current_sample, samples_to_read)) != samples_to_read) {
				error << string_compose(_("%1: could not write read raw data for peak computation (%2)"), _name, strerror (errno)) << endmsg;
				done_with_peakfile_writes (false);
				goto out;
			}

			lp.release(); // allow butler to refill buffers

			if (_session.deletion_in_progress() || _session.peaks_cleanup_in_progres()) {
				cerr << "peak file creation interrupted: " << _name << endmsg;
				lp.acquire();
				done_with_peakfile_writes (false);
				goto out;
			}

			if (compute_and_write_peaks (buf.get(), current_sample, samples_read, true, false, _FPP)) {
				break;
			}

			current_sample += samples_read;
			cnt -= samples_read;

			lp.acquire();
		}

		if (cnt == 0) {
			/* success */
			truncate_peakfile();
		}

		done_with_peakfile_writes ((cnt == 0));
		if (cnt == 0) {
			ret = 0;
		}
	}

  out:
	if (ret) {
		DEBUG_TRACE (DEBUG::Peaks, string_compose("Could not write peak data, attempting to remove peakfile %1\n", _peakpath));
		::g_unlink (_peakpath.c_str());
	}

	return ret;
}

int
AudioSource::close_peakfile ()
{
	WriterLock lp (_lock);
	if (-1 != _peakfile_fd) {
		close (_peakfile_fd);
		_peakfile_fd = -1;
	}
	if (!_peakpath.empty()) {
		::g_unlink (_peakpath.c_str());
	}
	_peaks_built = false;
	return 0;
}

int
AudioSource::prepare_for_peakfile_writes ()
{
	if (_session.deletion_in_progress() || _session.peaks_cleanup_in_progres() || 0 != (_flags & NoPeakFile)) {
		return -1;
	}

	if ((_peakfile_fd = g_open (_peakpath.c_str(), O_CREAT|O_RDWR, 0664)) == -1) {
		error << string_compose(_("AudioSource: cannot open _peakpath (c) \"%1\" (%2)"), _peakpath, strerror (errno)) << endmsg;
		return -1;
	}
	return 0;
}

void
AudioSource::done_with_peakfile_writes (bool done)
{
	if (_session.deletion_in_progress() || _session.peaks_cleanup_in_progres()) {
		if (-1 != _peakfile_fd) {
			close (_peakfile_fd);
			_peakfile_fd = -1;
		}
		return;
	}

	if (peak_leftover_cnt) {
		compute_and_write_peaks (0, 0, 0, true, false, _FPP);
	}

	if (-1 != _peakfile_fd) {
		close (_peakfile_fd);
		_peakfile_fd = -1;
	}

	if (done) {
		PBD::Mutex::Lock lm (_peaks_ready_lock);
		_peaks_built = true;
		PeaksReady (); /* EMIT SIGNAL */
	}
}

/** @param first_sample Offset from the source start of the first sample to
 * process. _lock MUST be held by caller.
*/
int
AudioSource::compute_and_write_peaks (Sample const * buf, samplecnt_t first_sample, samplecnt_t cnt,
				      bool force, bool intermediate_peaks_ready)
{
	return compute_and_write_peaks (buf, first_sample, cnt, force, intermediate_peaks_ready, _FPP);
}

int
AudioSource::compute_and_write_peaks (Sample const * buf, samplecnt_t first_sample, samplecnt_t cnt,
				      bool force, bool intermediate_peaks_ready, samplecnt_t fpp)
{
	samplecnt_t to_do;
	uint32_t  peaks_computed;
	samplepos_t current_sample;
	samplecnt_t samples_done;
	const size_t blocksize = (128 * 1024);
	off_t first_peak_byte;
	std::unique_ptr<Sample[]> buf2;

	if (-1 == _peakfile_fd) {
		if (prepare_for_peakfile_writes ()) {
			return -1;
		}
	}

  restart:
	if (peak_leftover_cnt) {

		if (first_sample != peak_leftover_sample + peak_leftover_cnt) {

			/* uh-oh, ::seek() since the last ::compute_and_write_peaks(),
			   and we have leftovers. flush a single peak (since the leftovers
			   never represent more than that, and restart.
			*/

			PeakData x;

			x.min = peak_leftovers[0];
			x.max = peak_leftovers[0];
			find_peaks (peak_leftovers, peak_leftover_cnt, &x.min, &x.max);
			x.rms = waveform_rms (peak_leftovers, peak_leftover_cnt);

			off_t byte = (peak_leftover_sample / fpp) * sizeof (PeakData);

			off_t offset = lseek (_peakfile_fd, byte, SEEK_SET);

			if (offset != byte) {
				error << string_compose(_("%1: could not seek in peak file data (%2)"), _name, strerror (errno)) << endmsg;
				return -1;
			}

			if (::write (_peakfile_fd, &x, sizeof (PeakData)) != sizeof (PeakData)) {
				error << string_compose(_("%1: could not write peak file data (%2)"), _name, strerror (errno)) << endmsg;
				return -1;
			}

			_peak_byte_max = max (_peak_byte_max, (off_t) (byte + sizeof(PeakData)));

			{
				PBD::Mutex::Lock lm (_peaks_ready_lock);
				PeakRangeReady (peak_leftover_sample, peak_leftover_cnt); /* EMIT SIGNAL */
				if (intermediate_peaks_ready) {
					PeaksReady (); /* EMIT SIGNAL */
				}
			}

			/* left overs are done */

			peak_leftover_cnt = 0;
			goto restart;
		}

		/* else ... had leftovers, but they immediately precede the new data, so just
		   merge them and compute.
		*/

		/* make a new contiguous buffer containing leftovers and the new stuff */

		to_do = cnt + peak_leftover_cnt;
		buf2.reset(new Sample[to_do]);

		/* the remnants */
		memcpy (buf2.get(), peak_leftovers, peak_leftover_cnt * sizeof (Sample));

		/* the new stuff */
		if (buf && cnt > 0) {
			memcpy (buf2.get()+peak_leftover_cnt, buf, cnt * sizeof (Sample));
		}

		/* no more leftovers */
		peak_leftover_cnt = 0;

		/* use the temporary buffer */
		buf = buf2.get();

		/* make sure that when we write into the peakfile, we startup where we left off */

		first_sample = peak_leftover_sample;

	} else {
		to_do = cnt;
	}

	std::unique_ptr<PeakData[]> peakbuf(new PeakData[(to_do/fpp)+1]);
	peaks_computed = 0;
	current_sample = first_sample;
	samples_done = 0;

	while (to_do) {

		/* if some samples were passed in (i.e. we're not flushing leftovers)
		   and there are less than fpp to do, save them till
		   next time
		*/

		if (force && (to_do < fpp)) {
			/* keep the left overs around for next time */

			if (peak_leftover_size < to_do) {
				delete [] peak_leftovers;
				peak_leftovers = new Sample[to_do];
				peak_leftover_size = to_do;
			}
			memcpy (peak_leftovers, buf, to_do * sizeof (Sample));
			peak_leftover_cnt = to_do;
			peak_leftover_sample = current_sample;

			/* done for now */

			break;
		}

		samplecnt_t this_time = min (fpp, to_do);

		peakbuf[peaks_computed].max = buf[0];
		peakbuf[peaks_computed].min = buf[0];

		ARDOUR::find_peaks (buf+1, this_time-1, &peakbuf[peaks_computed].min, &peakbuf[peaks_computed].max);

		peakbuf[peaks_computed].rms = waveform_rms (buf, this_time);
		peaks_computed++;
		buf += this_time;
		to_do -= this_time;
		samples_done += this_time;
		current_sample += this_time;
	}

	first_peak_byte = (first_sample / fpp) * sizeof (PeakData);

	if (can_truncate_peaks()) {

		/* on some filesystems (ext3, at least) this helps to reduce fragmentation of
		   the peakfiles. its not guaranteed to do so, and even on ext3 (as of december 2006)
		   it does not cause single-extent allocation even for peakfiles of
		   less than BLOCKSIZE bytes.  only call ftruncate if we'll make the file larger.
		*/

		off_t endpos = lseek (_peakfile_fd, 0, SEEK_END);
		off_t target_length = blocksize * ((first_peak_byte + blocksize + 1) / blocksize);

		if (endpos < target_length) {
			DEBUG_TRACE(DEBUG::Peaks, string_compose ("Truncating Peakfile A %1 to %2\n", _peakpath, target_length));
#ifdef COMPILER_MSVC
			if (_chsize_s (_peakfile_fd, target_length))
#else
			if (ftruncate (_peakfile_fd, target_length))
#endif
			{
				/* error doesn't actually matter so continue on without testing */
			}
		}
	}


	off_t offset = lseek(_peakfile_fd, first_peak_byte, SEEK_SET);

	if (offset != first_peak_byte) {
		error << string_compose(_("%1: could not seek in peak file data (%2)"), _name, strerror (errno)) << endmsg;
		return -1;
	}

	ssize_t bytes_to_write = sizeof (PeakData) * peaks_computed;

	ssize_t bytes_written = ::write (_peakfile_fd, peakbuf.get(), bytes_to_write);

	if (bytes_written != bytes_to_write) {
		error << string_compose(_("%1: could not write peak file data (%2)"), _name, strerror (errno)) << endmsg;
		return -1;
	}

	_peak_byte_max = max (_peak_byte_max, (off_t) (first_peak_byte + bytes_to_write));

	if (samples_done) {
		PBD::Mutex::Lock lm (_peaks_ready_lock);
		PeakRangeReady (first_sample, samples_done); /* EMIT SIGNAL */
		if (intermediate_peaks_ready) {
			PeaksReady (); /* EMIT SIGNAL */
		}
	}

	return 0;
}

void
AudioSource::truncate_peakfile ()
{
	if (-1 == _peakfile_fd) {
		error << string_compose (_("programming error: %1"), "AudioSource::truncate_peakfile() called without open peakfile descriptor")
		      << endmsg;
		return;
	}

	/* truncate the peakfile down to its natural length if necessary */

	off_t end = lseek (_peakfile_fd, 0, SEEK_END);

	if (end > _peak_byte_max) {
		DEBUG_TRACE(DEBUG::Peaks, string_compose ("Truncating Peakfile B %1 to %2\n", _peakpath, _peak_byte_max));
#ifdef COMPILER_MSVC
		if (_chsize_s (_peakfile_fd, _peak_byte_max))
#else
		if (ftruncate (_peakfile_fd, _peak_byte_max))
#endif
		{
			error << string_compose (_("could not truncate peakfile %1 to %2 (error: %3)"),
						 _peakpath, _peak_byte_max, errno) << endmsg;
		}
	}
}

samplecnt_t
AudioSource::available_peaks (double zoom_factor) const
{
	if (zoom_factor < _FPP) {
		return _length.samples(); // peak data will come from the audio file
	}

	/* peak data comes from peakfile, but the filesize might not represent
	   the valid data due to ftruncate optimizations, so use _peak_byte_max state.
	   XXX - there might be some atomicity issues here, we should probably add a lock,
	   but _peak_byte_max only monotonically increases after initialization.
	*/

	off_t end = _peak_byte_max;

	return (end/sizeof(PeakData)) * _FPP;
}

void
AudioSource::mark_streaming_write_completed (const WriterLock& lock, Temporal::timecnt_t const &)
{
	estimate_tempo ();

	{
		PBD::Mutex::Lock lm (_peaks_ready_lock);

		if (_peaks_built) {
			PeaksReady (); /* EMIT SIGNAL */
		}
	}
}

