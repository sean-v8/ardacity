/*
 * Copyright (C) 2005-2006 Nick Mainsbridge <mainsbridge@gmail.com>
 * Copyright (C) 2005-2006 Sampo Savolainen <v2@iki.fi>
 * Copyright (C) 2005-2006 Taybin Rutkin <taybin@taybin.com>
 * Copyright (C) 2005-2007 Doug McLain <doug@nostar.net>
 * Copyright (C) 2005-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2005 Karsten Wiese <fzuuzf@googlemail.com>
 * Copyright (C) 2006-2015 David Robillard <d@drobilla.net>
 * Copyright (C) 2007-2012 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2013-2014 John Emmas <john@creativepost.co.uk>
 * Copyright (C) 2014-2019 Robin Gareus <robin@gareus.org>
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
#include <cstdlib>
#include <cmath>
#include <cassert>

#include <algorithm>
#include <string>
#include <vector>

#include <sigc++/bind.h>

#include "pbd/error.h"
#include "pbd/stl_delete.h"
#include "pbd/memento_command.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/utils.h"

#include "widgets/ardour_button.h"

#include "ardour/event_type_map.h"
#include "ardour/logmeter.h"
#include "ardour/pannable.h"
#include "ardour/panner.h"
#include "ardour/panner_shell.h"

#include "audio_time_axis.h"
#include "editor_automation_line.h"
#include "enums.h"
#include "gui_thread.h"
#include "automation_time_axis.h"
#include "keyboard.h"
#include "playlist_selector.h"
#include "public_editor.h"
#include "audio_region_view.h"
#include "audio_streamview.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace ArdourWidgets;
using namespace PBD;
using namespace Gtk;
using namespace Editing;

AudioTimeAxisView::AudioTimeAxisView (PublicEditor& ed, Session* sess, ArdourCanvas::Canvas& canvas)
	: SessionHandlePtr (sess)
	, RouteTimeAxisView(ed, sess, canvas)
{
	UIConfiguration::instance().ParameterChanged.connect (sigc::mem_fun (*this, &AudioTimeAxisView::parameter_changed));
}

double
AudioTimeAxisView::waveform_value_at (double y) const
{
	const unsigned channels = std::max (1u, track ()->n_channels ().n_audio ());
	const double height = std::max (1.0, double (_waveform_ruler.get_height ()) / channels);
	const double local = std::fmod (std::max (0.0, y), height);
	return _waveform_high - local / height * (_waveform_high - _waveform_low);
}

bool
AudioTimeAxisView::draw_waveform_ruler (GdkEventExpose*)
{
	const unsigned channels = std::max (1u, track ()->n_channels ().n_audio ());
	const double height = double (_waveform_ruler.get_height ()) / channels;
	const double width = _waveform_ruler.get_width ();
	auto context = _waveform_ruler.get_window ()->create_cairo_context ();
	auto color = _waveform_ruler.get_style ()->get_fg (Gtk::STATE_NORMAL);
	context->set_source_rgb (color.get_red_p (), color.get_green_p (), color.get_blue_p ());
	context->set_line_width (1.0);
	auto layout = _waveform_ruler.create_pango_layout ("");
	const int intervals = height >= 84 ? 4 : (height >= 48 ? 2 : 1);
	const double tick_step = (_waveform_high - _waveform_low) / intervals;
	const int decimals = std::min (4, std::max (1, int (std::ceil (-std::log10 (tick_step)))));
	const double zero_threshold = 0.5 * std::pow (10.0, -decimals);
	for (unsigned channel = 0; channel < channels; ++channel) {
		for (int i = 0; i <= intervals; ++i) {
			const double fraction = double (i) / intervals;
			const double y = height * (channel + fraction);
			const double amplitude = _waveform_high - fraction * (_waveform_high - _waveform_low);
			char text[24];
			const double label_value = std::abs (amplitude) < zero_threshold ? 0.0 : amplitude;
			snprintf (text, sizeof (text), "%.*f", decimals, label_value);
			layout->set_text (text);
			int tw, th;
			layout->get_pixel_size (tw, th);
			context->move_to (width - 5, y + 0.5);
			context->line_to (width, y + 0.5);
			context->stroke ();
			const double label_y = std::max (height * channel,
				std::min (y - th * 0.5, height * (channel + 1) - th));
			context->move_to (std::max (0.0, width - tw - 7), label_y);
			layout->show_in_cairo_context (context);
		}
	}
	return true;
}

void
AudioTimeAxisView::set_waveform_range (double low, double high)
{
	low = std::max (-8.0, low);
	high = std::min (8.0, high);
	/* Leave one unit in the last displayed decimal place between ticks. */
	const double minimum_span = 0.0004;
	if (high - low < minimum_span) {
		const double center = (low + high) * 0.5;
		low = std::max (-8.0, std::min (8.0 - minimum_span, center - minimum_span * 0.5));
		high = low + minimum_span;
	}
	_waveform_low = low;
	_waveform_high = high;
	audio_view ()->set_waveform_range (low, high);
	_waveform_ruler.queue_draw ();
}

void
AudioTimeAxisView::zoom_waveform_to_peak (double amplitude, double low, double high)
{
	const bool logscaled = UIConfiguration::instance().get_waveform_scale () == ARDOUR::Logarithmic;
	auto display_value = [logscaled] (double value) {
		return logscaled && value != 0.0
			? std::copysign (alt_log_meter (fast_coefficient_to_dB (std::abs (value))), value)
			: value;
	};

	_pre_peak_low = _waveform_low;
	_pre_peak_high = _waveform_high;
	_waveform_peak_framed = true;
	_waveform_peak = std::max (-8.0, std::min (8.0, display_value (amplitude)));
	const double below = _waveform_peak < 0.0 ? 0.05 : 0.95;
	const double span = std::max ((_waveform_peak - display_value (low)) / below,
	                              (display_value (high) - _waveform_peak) / (1.0 - below)) / 0.95;
	step_waveform_peak_zoom (span / (_waveform_high - _waveform_low));
}

void
AudioTimeAxisView::restore_waveform_peak_zoom ()
{
	if (_waveform_peak_framed) {
		_waveform_peak_framed = false;
		set_waveform_range (_pre_peak_low, _pre_peak_high);
	}
}

void
AudioTimeAxisView::step_waveform_peak_zoom (double factor)
{
	if (!_waveform_peak_framed) {
		return;
	}
	const double span = std::max (0.0004, std::min (16.0, (_waveform_high - _waveform_low) * factor));
	const double below = _waveform_peak < 0.0 ? 0.05 : 0.95;
	const double low = std::max (-8.0, std::min (8.0 - span, _waveform_peak - below * span));
	set_waveform_range (low, low + span);
}

void
AudioTimeAxisView::zoom_waveform_at (double center, double factor)
{
	const double half_span = (_waveform_high - _waveform_low) * factor * 0.5;
	set_waveform_range (center - half_span, center + half_span);
}

bool
AudioTimeAxisView::waveform_ruler_press (GdkEventButton* event)
{
	const bool shift = (event->state & GDK_SHIFT_MASK) != 0;
	const double center = waveform_value_at (event->y);
	if (event->button == 1) {
		_waveform_press_y = event->y;
		return true;
	}
	if (event->button != 3) {
		return true;
	}
	if (shift) {
		set_waveform_range (-1, 1);
		return true;
	}
	for (auto* child : _waveform_menu.get_children ()) {
		_waveform_menu.remove (*child);
	}
	auto add_item = [this] (const char* label, const auto& slot) {
		auto* item = Gtk::manage (new Gtk::MenuItem (label));
		item->signal_activate ().connect (slot);
		_waveform_menu.append (*item);
		item->show ();
	};
	add_item (_("Reset Waveform Zoom"), [this] { set_waveform_range (-1, 1); });
	add_item (_("Zoom In ×2"), [this, center] { zoom_waveform_at (center, 0.5); });
	add_item (_("Zoom Out ×½"), [this, center] { zoom_waveform_at (center, 2.0); });
	add_item (_("Half Wave"), [this] { set_waveform_range (0, 1); });
	_waveform_menu.popup (event->button, event->time);
	return true;
}

bool
AudioTimeAxisView::waveform_ruler_release (GdkEventButton* event)
{
	if (event->button == 1 && _waveform_press_y >= 0) {
		const double start = waveform_value_at (_waveform_press_y);
		const double end = waveform_value_at (event->y);
		if (std::abs (event->y - _waveform_press_y) >= 4 && start != end) {
			set_waveform_range (std::min (start, end), std::max (start, end));
		} else {
			zoom_waveform_at (start, (event->state & GDK_SHIFT_MASK) ? 2.0 : 0.5);
		}
		_waveform_press_y = -1.0;
	}
	return true;
}

void
AudioTimeAxisView::set_route (std::shared_ptr<Route> rt)
{
	_route = rt;

	/* RouteTimeAxisView::set_route() sets up some things in the View,
	   so it must be created before RouteTimeAxis::set_route() is
	   called.
	*/
	_view = new AudioStreamView (*this);

	RouteTimeAxisView::set_route (rt);

	_view->apply_color (Gtkmm2ext::gdk_color_to_rgba (color()), StreamView::RegionColor);

	// Make sure things are sane...
	assert(!is_track() || is_audio_track());

	/* if set_state above didn't create a gain automation child, we need to make one */
	if (automation_child (GainAutomation) == 0) {
		create_automation_child (GainAutomation, false);
	}

	if (automation_child (TrimAutomation) == 0) {
		create_automation_child (TrimAutomation, false);
	}

	/* if set_state above didn't create a mute automation child, we need to make one */
	if (automation_child (MuteAutomation) == 0) {
		create_automation_child (MuteAutomation, false);
	}

	if (_route->panner_shell()) {
		_route->panner_shell()->Changed.connect (*this, invalidator (*this),
		                                         std::bind (&AudioTimeAxisView::ensure_pan_views, this, false), gui_context());
	}

	/* map current state of the route */

	processors_changed (RouteProcessorChange ());
	reset_processor_automation_curves ();
	ensure_pan_views (false);
	update_control_names ();

	if (is_audio_track()) {
		_waveform_ruler.set_size_request (48, -1);
		_waveform_ruler.add_events (Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK);
		_waveform_ruler.signal_expose_event ().connect (sigc::mem_fun (*this, &AudioTimeAxisView::draw_waveform_ruler));
		_waveform_ruler.signal_button_press_event ().connect (sigc::mem_fun (*this, &AudioTimeAxisView::waveform_ruler_press));
		_waveform_ruler.signal_button_release_event ().connect (sigc::mem_fun (*this, &AudioTimeAxisView::waveform_ruler_release));
		time_axis_hbox.pack_end (_waveform_ruler, false, false);
		_waveform_ruler.show ();

		/* ask for notifications of any new RegionViews */
		_view->RegionViewAdded.connect (sigc::mem_fun(*this, &AudioTimeAxisView::region_view_added));

		if (!_editor.have_idled()) {
			/* first idle will do what we need */
		} else {
			first_idle ();
		}

	} else {
		post_construct ();
	}
}

AudioTimeAxisView::~AudioTimeAxisView ()
{
	delete _view;
	_view = nullptr;
}

void
AudioTimeAxisView::first_idle ()
{
	_view->attach ();
	post_construct ();
}

AudioStreamView*
AudioTimeAxisView::audio_view()
{
	return dynamic_cast<AudioStreamView*>(_view);
}

void
AudioTimeAxisView::create_automation_child (const Evoral::Parameter& param, bool show)
{
	if (param.type() == NullAutomation) {
		return;
	}

	AutomationTracks::iterator existing = _automation_tracks.find (param);

	if (existing != _automation_tracks.end()) {

		/* automation track created because we had existing data for
		 * the processor, but visibility may need to be controlled
		 * since it will have been set visible by default.
		 */

		existing->second->set_marked_for_display (show);

		if (!no_redraw) {
			request_redraw ();
		}

		return;
	}

	if (param.type() == GainAutomation) {

		create_gain_automation_child (param, show);

	} else if (param.type() == TrimAutomation) {

		create_trim_automation_child (param, show);

	} else if (param.type() == PanWidthAutomation ||
	           param.type() == PanElevationAutomation ||
	           param.type() == PanAzimuthAutomation) {

		ensure_pan_views (show);

	} else if (param.type() == PluginAutomation) {

		/* handled elsewhere */

	} else if (param.type() == MuteAutomation) {

		create_mute_automation_child (param, show);


	} else {
		warning << "AudioTimeAxisView: unknown automation child " << EventTypeMap::instance().to_symbol(param) << endmsg;
	}
}

void
AudioTimeAxisView::show_all_automation (bool apply_to_selection)
{
	if (apply_to_selection) {
		_editor.get_selection().tracks.foreach_audio_time_axis (std::bind (&AudioTimeAxisView::show_all_automation, _1, false));
	} else {

		no_redraw = true;

		RouteTimeAxisView::show_all_automation ();

		no_redraw = false;
		request_redraw ();
	}
}

void
AudioTimeAxisView::show_existing_automation (bool apply_to_selection)
{
	if (apply_to_selection) {
		_editor.get_selection().tracks.foreach_audio_time_axis (std::bind (&AudioTimeAxisView::show_existing_automation, _1, false));
	} else {
		no_redraw = true;

		RouteTimeAxisView::show_existing_automation ();

		no_redraw = false;

		request_redraw ();
	}
}

void
AudioTimeAxisView::hide_all_automation (bool apply_to_selection)
{
	if (apply_to_selection) {
		_editor.get_selection().tracks.foreach_audio_time_axis (std::bind (&AudioTimeAxisView::hide_all_automation, _1, false));
	} else {
		no_redraw = true;

		RouteTimeAxisView::hide_all_automation();

		no_redraw = false;
		request_redraw ();
	}
}

void
AudioTimeAxisView::route_active_changed ()
{
	RouteTimeAxisView::route_active_changed();
	update_control_names ();

	if (!_route->active()) {
		controls_table.hide();
		inactive_table.show();
		RouteTimeAxisView::hide_all_automation();
	} else {
		inactive_table.hide();
		controls_table.show();
	}
}

void
AudioTimeAxisView::parameter_changed (string const & p)
{
	if (p == "vertical-region-gap") {
		_view->update_contents_height ();
	}
}


/**
 *    Set up the names of the controls so that they are coloured
 *    correctly depending on whether this route is inactive or
 *    selected.
 */

void
AudioTimeAxisView::update_control_names ()
{
	if (is_audio_track()) {
		if (_route->is_transient() && _route->active()) {
			controls_base_selected_name = "TansientTrackBaseSelected";
			controls_base_unselected_name = "TansientTrackBaseUnselected";
		} else if (_route->active()) {
			controls_base_selected_name = "AudioTrackControlsBaseSelected";
			controls_base_unselected_name = "AudioTrackControlsBaseUnselected";
		} else {
			controls_base_selected_name = "AudioTrackControlsBaseInactiveSelected";
			controls_base_unselected_name = "AudioTrackControlsBaseInactiveUnselected";
		}
	} else {
		if (_route->active()) {
			controls_base_selected_name = "BusControlsBaseSelected";
			controls_base_unselected_name = "BusControlsBaseUnselected";
		} else {
			controls_base_selected_name = "BusControlsBaseInactiveSelected";
			controls_base_unselected_name = "BusControlsBaseInactiveUnselected";
		}
	}

	if (selected()) {
		controls_ebox.set_name (controls_base_selected_name);
		time_axis_frame.set_name (controls_base_selected_name);
	} else {
		controls_ebox.set_name (controls_base_unselected_name);
		time_axis_frame.set_name (controls_base_unselected_name);
	}
}

void
AudioTimeAxisView::route_property_changed (PBD::PropertyChange const& what_changed)
{
	RouteTimeAxisView::route_property_changed (what_changed);
	if (what_changed.contains (Properties::transient)) {
		update_control_names ();
	}
}

void
AudioTimeAxisView::build_automation_action_menu (bool for_selection)
{
	RouteTimeAxisView::build_automation_action_menu (for_selection);
}
