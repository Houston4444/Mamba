/*
 *                           0BSD 
 * 
 *                    BSD Zero Clause License
 * 
 *  Copyright (c) 2020 Hermann Meyer
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.

 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 */


#include "MidiKeyBoard.h"
#include "xkeyboard.h"

//   g++ -Wall NsmHandler.cpp MidiKeyBoard.cpp xkeyboard.c -L. ../libxputty/libxputty/libxputty.a -o midikeyboard -I./ -I../libxputty/libxputty/include/ `pkg-config --cflags --libs jack cairo x11 sigc++-2.0 liblo` -lm -pthread

namespace midikeyboard {


/****************************************************************
 ** class AnimatedKeyBoard
 **
 ** animate midi input from jack on the keyboard in a extra thread
 ** 
 */

AnimatedKeyBoard::AnimatedKeyBoard() 
    :_execute(false) {
}

AnimatedKeyBoard::~AnimatedKeyBoard() {
    if( _execute.load(std::memory_order_acquire) ) {
        stop();
    };
}

void AnimatedKeyBoard::stop() {
    _execute.store(false, std::memory_order_release);
    if (_thd.joinable()) {
        _thd.join();
    }
}

void AnimatedKeyBoard::start(int interval, std::function<void(void)> func) {
    if( _execute.load(std::memory_order_acquire) ) {
        stop();
    };
    _execute.store(true, std::memory_order_release);
    _thd = std::thread([this, interval, func]() {
        while (_execute.load(std::memory_order_acquire)) {
            func();                   
            std::this_thread::sleep_for(
                std::chrono::milliseconds(interval));
        }
    });
}

bool AnimatedKeyBoard::is_running() const noexcept {
    return ( _execute.load(std::memory_order_acquire) && 
             _thd.joinable() );
}


/****************************************************************
 ** class MidiMessenger
 **
 ** create, collect and send all midi events to jack_midi out buffer
 */

MidiMessenger::MidiMessenger() {
    channel = 0;
    for (int i = 0; i < max_midi_cc_cnt; i++) {
        send_cc[i] = false;
    }
}

inline int MidiMessenger::next(int i) const {
    while (++i < max_midi_cc_cnt) {
        if (send_cc[i].load(std::memory_order_acquire)) {
            return i;
        }
    }
    return -1;
}

inline void MidiMessenger::fill(unsigned char *midi_send, int i) {
    if (size(i) == 3) {
        midi_send[2] =  bg_num[i];
    }
    midi_send[1] = pg_num[i];    // program value
    midi_send[0] = cc_num[i];    // controller+ channel
    send_cc[i].store(false, std::memory_order_release);
}

bool MidiMessenger::send_midi_cc(int _cc, int _pg, int _bgn, int _num) {
    _cc |=channel;
    for(int i = 0; i < max_midi_cc_cnt; i++) {
        if (send_cc[i].load(std::memory_order_acquire)) {
            if (cc_num[i] == _cc && pg_num[i] == _pg &&
                bg_num[i] == _bgn && me_num[i] == _num)
                return true;
        } else if (!send_cc[i].load(std::memory_order_acquire)) {
            cc_num[i] = _cc;
            pg_num[i] = _pg;
            bg_num[i] = _bgn;
            me_num[i] = _num;
            send_cc[i].store(true, std::memory_order_release);
            return true;
        }
    }
    return false;
}


/****************************************************************
 ** class MidiRecord
 **
 ** record the keyboard input in a extra thread
 ** 
 */

MidiRecord::MidiRecord() 
    :_execute(false) {
    st = NULL;
}

MidiRecord::~MidiRecord() {
    if( _execute.load(std::memory_order_acquire) ) {
        stop();
    };
}

void MidiRecord::stop() {
    _execute.store(false, std::memory_order_release);
    if (_thd.joinable()) {
        cv.notify_one();
        _thd.join();
    }
}

void MidiRecord::start() {
    if( _execute.load(std::memory_order_acquire) ) {
        stop();
    };
    _execute.store(true, std::memory_order_release);
    _thd = std::thread([this]() {
        while (_execute.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk);
            play.reserve(play.size() + st->size());
            
            for (unsigned int i=0; i<st->size(); i++) 
                play.push_back((*st)[i]); 
            st->clear();
        }
    });
}

bool MidiRecord::is_running() const noexcept {
    return ( _execute.load(std::memory_order_acquire) && 
             _thd.joinable() );
}


/****************************************************************
 ** class XJack
 **
 ** Send content from MidiMessenger to jack_midi out
 ** pass all incoming midi events to jack_midi out.
 ** send all incomming midi events to XKeyBoard
 */

XJack::XJack(MidiMessenger *mmessage_)
    : mmessage(mmessage_),
     start(0),
     stop(0),
     deltaTime(0),
     client(NULL),
     rec() {
        transport_state = JackTransportStopped;
        old_transport_state = JackTransportStopped;
        record = 0;
        play = 0;
        fresh_take = true;
        first_play = true;
        store1.reserve(256);
        store2.reserve(256);
        st = &store1;
        rec.st = &store1;
        client_name = "Mamba";
}

XJack::~XJack() {
    if (client) jack_client_close (client);
    if (rec.is_running()) rec.stop();
}

void XJack::init_jack() {
    if ((client = jack_client_open (client_name.c_str(), JackNullOption, NULL)) == 0) {
        fprintf (stderr, "jack server not running?\n");
        trigger_quit_by_jack();
    }

    in_port = jack_port_register(
                  client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    out_port = jack_port_register(
                   client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    jack_set_xrun_callback(client, jack_xrun_callback, this);
    jack_set_sample_rate_callback(client, jack_srate_callback, this);
    jack_set_buffer_size_callback(client, jack_buffersize_callback, this);
    jack_set_process_callback(client, jack_process, this);
    jack_on_shutdown (client, jack_shutdown, this);

    if (jack_activate (client)) {
        fprintf (stderr, "cannot activate client");
        trigger_quit_by_jack();
    }

    if (!jack_is_realtime(client)) {
        fprintf (stderr, "jack isn't running with realtime priority\n");
    } else {
        fprintf (stderr, "jack running with realtime priority\n");
    }
}

// jack process callback for the midi output
inline void XJack::process_midi_out(void *buf, jack_nframes_t nframes) {
    int i = mmessage->next();
    for (unsigned int n = 0; n < nframes; n++) {
        if (record) {
            if (fresh_take) start = jack_last_frame_time(client);
            fresh_take = false;
        }
        if (i >= 0) {
            unsigned char* midi_send = jack_midi_event_reserve(buf, n, mmessage->size(i));
            if (midi_send) {
                mmessage->fill(midi_send, i);
                if (record) {
                    stop = jack_last_frame_time(client);
                    deltaTime = (double)(stop-start);
                    unsigned char d = mmessage->size(i) > 2 ? midi_send[2] : 0;
                    MidiEvent ev = {midi_send[0], midi_send[1], d, mmessage->size(i), deltaTime};
                    st->push_back(ev);
                    if (store1.size() >= 256) {
                        st = &store2;
                        rec.st = &store1;
                        rec.cv.notify_one();
                    } else if (store2.size() >= 256) {
                        st = &store1;
                        rec.st = &store2;
                        rec.cv.notify_one();
                    }
                    start = jack_last_frame_time(client);
                }
            }
            i = mmessage->next(i);
        } else if (play && rec.play.size()) {
            static unsigned int p = 0;
            if (first_play) {
                start = jack_last_frame_time(client);
                first_play = false;
                p = 0;
            }
            stop = jack_last_frame_time(client);
            deltaTime = (double)(stop-start);
            MidiEvent ev = rec.play[p];
            if (deltaTime >= ev.deltaTime) {
                unsigned char* midi_send = jack_midi_event_reserve(buf, n, ev.me_num);
                if (midi_send) {
                    midi_send[0] = ev.cc_num;
                    midi_send[1] = ev.pg_num;
                    if(ev.me_num > 2)
                        midi_send[2] = ev.bg_num;
                    bool ch = true;
                    if ((mmessage->channel) != (int(ev.cc_num&0x0f))) {
                        ch = false;
                    }
                    if ((ev.cc_num & 0xf0) == 0x90 && ch) {   // Note On
                        std::async(std::launch::async, trigger_get_midi_in, ev.pg_num, true);
                    } else if ((ev.cc_num & 0xf0) == 0x80 && ch) {   // Note Off
                        std::async(std::launch::async, trigger_get_midi_in, ev.pg_num, false);
                    }
                }
                start = jack_last_frame_time(client);
                p++;
                if (p>=rec.play.size()) p = 0;
            }
        }
        
    }
}

// jack process callback for the midi input
void XJack::process_midi_in(void* buf, void *arg) {
    XJack *xjack = (XJack*)arg;
    jack_midi_event_t in_event;
    jack_nframes_t event_count = jack_midi_get_event_count(buf);
    unsigned int i;
    for (i = 0; i < event_count; i++) {
        jack_midi_event_get(&in_event, buf, i);
        bool ch = true;
        if ((xjack->mmessage->channel) != (int(in_event.buffer[0]&0x0f))) {
            ch = false;
        }
        if ((in_event.buffer[0] & 0xf0) == 0x90 && ch) {   // Note On
            std::async(std::launch::async, xjack->trigger_get_midi_in, in_event.buffer[1], true);
        } else if ((in_event.buffer[0] & 0xf0) == 0x80 && ch) {   // Note Off
            std::async(std::launch::async, xjack->trigger_get_midi_in, in_event.buffer[1], false);
        }
    }
}

// static
void XJack::jack_shutdown (void *arg) {
    XJack *xjack = (XJack*)arg;
    xjack->trigger_quit_by_jack();
}

// static
int XJack::jack_xrun_callback(void *arg) {
    fprintf (stderr, "Xrun \r");
    return 0;
}

// static
int XJack::jack_srate_callback(jack_nframes_t samplerate, void* arg) {
    fprintf (stderr, "Samplerate %iHz \n", samplerate);
    return 0;
}

// static
int XJack::jack_buffersize_callback(jack_nframes_t nframes, void* arg) {
    fprintf (stderr, "Buffersize is %i samples \n", nframes);
    return 0;
}

// static
int XJack::jack_process(jack_nframes_t nframes, void *arg) {
    XJack *xjack = (XJack*)arg;
    xjack->transport_state = jack_transport_query (xjack->client, &xjack->current);
    void *in = jack_port_get_buffer (xjack->in_port, nframes);
    void *out = jack_port_get_buffer (xjack->out_port, nframes);
    xjack->process_midi_in(in, arg);
    memcpy (out, in,
        sizeof (unsigned char) * nframes);
    xjack->process_midi_out(out,nframes);
    return 0;
}


/****************************************************************
 ** class XKeyBoard
 **
 ** Create Keyboard layout and send events to MidiMessenger
 ** 
 */

XKeyBoard::XKeyBoard(XJack *xjack_, MidiMessenger *mmessage_,
        nsmhandler::NsmSignalHandler& nsmsig_, AnimatedKeyBoard * animidi_)
    : xjack(xjack_),
    mmessage(mmessage_),
    animidi(animidi_),
    nsmsig(nsmsig_),
    icon(NULL) {
    client_name = "Mamba";
    if (getenv("XDG_CONFIG_HOME")) {
        path = getenv("XDG_CONFIG_HOME");
        config_file = path +"/Mamba.conf";
    } else {
        path = getenv("HOME");
        config_file = path +"/.config/Mamba.conf";
    }
    has_config = false;
    main_x = 0;
    main_y = 0;
    main_w = 700;
    main_h = 240;
    visible = 1;
    velocity = 127;
    mbank = 0;
    mprogram = 0;
    keylayout = 0;
    mchannel = 0;
    run_one_more = 0;

    nsmsig.signal_trigger_nsm_show_gui().connect(
        sigc::mem_fun(this, &XKeyBoard::nsm_show_ui));

    nsmsig.signal_trigger_nsm_hide_gui().connect(
        sigc::mem_fun(this, &XKeyBoard::nsm_hide_ui));

    nsmsig.signal_trigger_nsm_save_gui().connect(
        sigc::mem_fun(this, &XKeyBoard::save_config));

    nsmsig.signal_trigger_nsm_gui_open().connect(
        sigc::mem_fun(this, &XKeyBoard::set_config));

    xjack->signal_trigger_get_midi_in().connect(
        sigc::mem_fun(this, &XKeyBoard::get_midi_in));

    xjack->signal_trigger_quit_by_jack().connect(
        sigc::mem_fun(this, &XKeyBoard::quit_by_jack));
}

XKeyBoard::~XKeyBoard() {
    if (icon) {
        XFreePixmap(win->app->dpy, (*icon));
        icon = NULL;
    }
}

// GUI stuff starts here
void XKeyBoard::set_config(const char *name, const char *client_id, bool op_gui) {
    client_name = client_id;
    xjack->client_name = client_name;
    path = name;
    config_file = path + ".config";
    if (op_gui) {
        visible = 0;
    } else {
        visible = 1;
    }
}


void XKeyBoard::read_config() {
    std::ifstream infile(config_file);
    std::string line;
    if (infile.is_open()) {
        getline( infile, line );
        if (!line.empty()) main_x = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) main_y = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) main_w = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) main_h = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) visible = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) keylayout = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) mchannel = std::stoi(line);
        getline( infile, line );
        if (!line.empty()) velocity = std::stoi(line);
        infile.close();
        has_config = true;
    }
}

void XKeyBoard::save_config() {
    if(nsmsig.nsm_session_control)
        XLockDisplay(win->app->dpy);
    std::ofstream outfile(config_file);
     if (outfile.is_open()) {
         outfile << main_x << std::endl;
         outfile << main_y << std::endl;
         outfile << main_w << std::endl;
         outfile << main_h << std::endl;
         outfile << visible << std::endl;
         outfile << keylayout << std::endl;
         outfile << mchannel << std::endl;
         outfile << velocity << std::endl;
         outfile.close();
     }
    if(nsmsig.nsm_session_control)
        XUnlockDisplay(win->app->dpy);
}

void XKeyBoard::nsm_show_ui() {
    XLockDisplay(win->app->dpy);
    widget_show_all(win);
    XFlush(win->app->dpy);
    XMoveWindow(win->app->dpy,win->widget, main_x, main_y);
    nsmsig.trigger_nsm_gui_is_shown();
    XUnlockDisplay(win->app->dpy);
}

void XKeyBoard::nsm_hide_ui() {
    XLockDisplay(win->app->dpy);
    widget_hide(win);
    XFlush(win->app->dpy);
    nsmsig.trigger_nsm_gui_is_hidden();
    XUnlockDisplay(win->app->dpy);
}

void XKeyBoard::show_ui(int present) {
    if(present) {
        widget_show_all(win);
        XMoveWindow(win->app->dpy,win->widget, main_x, main_y);
        if(nsmsig.nsm_session_control)
            nsmsig.trigger_nsm_gui_is_shown();
    }else {
        widget_hide(win);
        if(nsmsig.nsm_session_control)
            nsmsig.trigger_nsm_gui_is_hidden();
    }
}

void XKeyBoard::get_midi_in(int n, bool on) {
    MidiKeyboard *keys = (MidiKeyboard*)wid->parent_struct;
    set_key_in_matrix(keys->in_key_matrix, n, on);
}

void XKeyBoard::quit_by_jack() {
    XLockDisplay(win->app->dpy);
    quit(win);
    XFlush(win->app->dpy);
    XUnlockDisplay(win->app->dpy);
}

// static
void XKeyBoard::mk_draw_knob(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    XWindowAttributes attrs;
    XGetWindowAttributes(w->app->dpy, (Window)w->widget, &attrs);
    int width = attrs.width-2;
    int height = attrs.height-2;

    const double scale_zero = 20 * (M_PI/180); // defines "dead zone" for knobs
    int arc_offset = 2;
    int knob_x = 0;
    int knob_y = 0;

    int grow = (width > height) ? height:width;
    knob_x = grow-1;
    knob_y = grow-1;
    /** get values for the knob **/

    int knobx = (width - knob_x) * 0.5;
    int knobx1 = width* 0.5;

    int knoby = (height - knob_y) * 0.5;
    int knoby1 = height * 0.5;

    double knobstate = adj_get_state(w->adj_y);
    double angle = scale_zero + knobstate * 2 * (M_PI - scale_zero);

    double pointer_off =knob_x/3.5;
    double radius = min(knob_x-pointer_off, knob_y-pointer_off) / 2;
    double lengh_x = (knobx+radius+pointer_off/2) - radius * sin(angle);
    double lengh_y = (knoby+radius+pointer_off/2) + radius * cos(angle);
    double radius_x = (knobx+radius+pointer_off/2) - radius/ 1.18 * sin(angle);
    double radius_y = (knoby+radius+pointer_off/2) + radius/ 1.18 * cos(angle);
    cairo_pattern_t* pat;
    cairo_new_path (w->crb);

    pat = cairo_pattern_create_linear (0, 0, 0, knob_y);
    cairo_pattern_add_color_stop_rgba (pat, 1,  0.3, 0.3, 0.3, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.75,  0.2, 0.2, 0.2, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.5,  0.15, 0.15, 0.15, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.25,  0.1, 0.1, 0.1, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0,  0.05, 0.05, 0.05, 1.0);

    cairo_scale (w->crb, 0.95, 1.05);
    cairo_arc(w->crb,knobx1+arc_offset/2, knoby1-arc_offset, knob_x/2.2, 0, 2 * M_PI );
    cairo_set_source (w->crb, pat);
    cairo_fill_preserve (w->crb);
     cairo_set_source_rgb (w->crb, 0.1, 0.1, 0.1); 
    cairo_set_line_width(w->crb,1);
    cairo_stroke(w->crb);
    cairo_scale (w->crb, 1.05, 0.95);
    cairo_new_path (w->crb);
    cairo_pattern_destroy (pat);
    pat = NULL;

    pat = cairo_pattern_create_linear (0, 0, 0, knob_y);
    cairo_pattern_add_color_stop_rgba (pat, 0,  0.3, 0.3, 0.3, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.25,  0.2, 0.2, 0.2, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.5,  0.15, 0.15, 0.15, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 0.75,  0.1, 0.1, 0.1, 1.0);
    cairo_pattern_add_color_stop_rgba (pat, 1,  0.05, 0.05, 0.05, 1.0);

    cairo_arc(w->crb,knobx1, knoby1, knob_x/2.6, 0, 2 * M_PI );
    cairo_set_source (w->crb, pat);
    cairo_fill_preserve (w->crb);
     cairo_set_source_rgb (w->crb, 0.1, 0.1, 0.1); 
    cairo_set_line_width(w->crb,1);
    cairo_stroke(w->crb);
    cairo_new_path (w->crb);
    cairo_pattern_destroy (pat);

    /** create a rotating pointer on the kob**/
    cairo_set_line_cap(w->crb, CAIRO_LINE_CAP_ROUND); 
    cairo_set_line_join(w->crb, CAIRO_LINE_JOIN_BEVEL);
    cairo_move_to(w->crb, radius_x, radius_y);
    cairo_line_to(w->crb,lengh_x,lengh_y);
    cairo_set_line_width(w->crb,3);
    cairo_set_source_rgb (w->crb,0.63,0.63,0.63);
    cairo_stroke(w->crb);
    cairo_new_path (w->crb);

    cairo_text_extents_t extents;
    /** show value on the kob**/
    if (w->state) {
        char s[64];
        snprintf(s, 63,"%d",  (int) w->adj_y->value);
        cairo_set_source_rgb (w->crb, 0.6, 0.6, 0.6);
        cairo_set_font_size (w->crb, knobx1/3);
        cairo_text_extents(w->crb, s, &extents);
        cairo_move_to (w->crb, knobx1-extents.width/2, knoby1+extents.height/2);
        cairo_show_text(w->crb, s);
        cairo_new_path (w->crb);
    }

    /** show label below the knob**/
    use_text_color_scheme(w, get_color_state(w));
    cairo_set_font_size (w->crb,  (w->app->normal_font-1)/w->scale.ascale);
    cairo_text_extents(w->crb,w->label , &extents);

    cairo_move_to (w->crb, knobx1-extents.width/2, height );
    cairo_show_text(w->crb, w->label);
    cairo_new_path (w->crb);
}

void XKeyBoard::draw_board(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    XWindowAttributes attrs;
    XGetWindowAttributes(w->app->dpy, (Window)w->widget, &attrs);
    int width = attrs.width;
    set_pattern(w,&w->app->color_scheme->selected,&w->app->color_scheme->normal,BACKGROUND_);
    cairo_paint (w->crb);
    use_bg_color_scheme(w, NORMAL_);
    cairo_rectangle(w->crb,0,0,width,40);
    cairo_fill (w->crb);

    use_fg_color_scheme(w, SELECTED_);
    cairo_rectangle(w->crb,0,117,width,2);
    cairo_fill_preserve (w->crb);
    use_bg_color_scheme(w, ACTIVE_);
    cairo_set_line_width(w->crb, 1.0);
    cairo_stroke(w->crb);

    cairo_rectangle(w->crb,0,38,width,2);
    cairo_fill_preserve (w->crb);
    cairo_stroke(w->crb);
}

Widget_t *XKeyBoard::add_keyboard_knob(Widget_t *parent, const char * label,
                                int x, int y, int width, int height) {
    Widget_t *wid = add_knob(parent,label, x, y, width, height);
    wid->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    set_adjustment(wid->adj,64.0, 64.0, 0.0, 127.0, 1.0, CL_CONTINUOS);
    wid->func.expose_callback = mk_draw_knob;
    wid->func.key_press_callback = key_press;
    wid->func.key_release_callback = key_release;
    return wid;
}

void XKeyBoard::init_ui(Xputty *app) {
    win = create_window(app, DefaultRootWindow(app->dpy), 0, 0, 700, 240);
    XSelectInput(win->app->dpy, win->widget,StructureNotifyMask|ExposureMask|KeyPressMask 
                    |EnterWindowMask|LeaveWindowMask|ButtonReleaseMask|KeyReleaseMask
                    |ButtonPressMask|Button1MotionMask|PointerMotionMask);
    widget_set_icon_from_png(win,icon,LDVAR(midikeyboard_png));
    std::string tittle = client_name + " - Virtual Midi Keyboard";
    widget_set_title(win, tittle.c_str());
    win->flags |= HAS_MEM | NO_AUTOREPEAT;
    win->scale.gravity = NORTHEAST;
    win->parent_struct = this;
    win->func.expose_callback = draw_board;
    win->func.configure_notify_callback = win_configure_callback;
    win->func.mem_free_callback = win_mem_free;
    win->func.map_notify_callback = map_callback;
    win->func.unmap_notify_callback = unmap_callback;
    win->func.key_press_callback = key_press;
    win->func.key_release_callback = key_release;

    XSizeHints* win_size_hints;
    win_size_hints = XAllocSizeHints();
    win_size_hints->flags =  PMinSize|PBaseSize|PMaxSize|PWinGravity|PResizeInc;
    win_size_hints->min_width = 700;
    win_size_hints->min_height = 240;
    win_size_hints->base_width = 700;
    win_size_hints->base_height = 240;
    win_size_hints->max_width = 1875;
    win_size_hints->max_height = 241; //need to be 1 more then min to avoid flicker in the UI!!
    win_size_hints->width_inc = 25;
    win_size_hints->height_inc = 0;
    win_size_hints->win_gravity = CenterGravity;
    XSetWMNormalHints(win->app->dpy, win->widget, win_size_hints);
    XFree(win_size_hints);


    Widget_t * tmp = add_label(win,"Channel:",10,5,60,20);
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;
    channel =  add_combobox(win, "Channel", 70, 5, 60, 30);
    channel->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    channel->scale.gravity = ASPECT;
    combobox_add_numeric_entrys(channel,1,16);
    combobox_set_active_entry(channel, 0);
    set_adjustment(channel->adj,0.0, 0.0, 0.0, 15.0, 1.0, CL_ENUM);
    channel->func.value_changed_callback = channel_callback;
    channel->func.key_press_callback = key_press;
    channel->func.key_release_callback = key_release;
    tmp = channel->childlist->childs[0];
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;

    tmp = add_label(win,"Bank:",140,5,60,20);
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;
    bank =  add_combobox(win, "Bank", 200, 5, 60, 30);
    bank->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    bank->scale.gravity = ASPECT;
    combobox_add_numeric_entrys(bank,0,127);
    combobox_set_active_entry(bank, 0);
    set_adjustment(bank->adj,0.0, 0.0, 0.0, 15.0, 1.0, CL_ENUM);
    bank->func.value_changed_callback = bank_callback;
    bank->func.key_press_callback = key_press;
    bank->func.key_release_callback = key_release;
    tmp = bank->childlist->childs[0];
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;

    tmp = add_label(win,"Program:",260,5,60,20);
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;
    program =  add_combobox(win, "Program", 320, 5, 60, 30);
    program->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    program->scale.gravity = ASPECT;
    combobox_add_numeric_entrys(program,0,127);
    combobox_set_active_entry(program, 0);
    set_adjustment(program->adj,0.0, 0.0, 0.0, 15.0, 1.0, CL_ENUM);
    program->func.value_changed_callback = program_callback;
    program->func.key_press_callback = key_press;
    program->func.key_release_callback = key_release;
    tmp = program->childlist->childs[0];
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;

    layout = add_combobox(win, "", 390, 5, 130, 30);
    layout->data = LAYOUT;
    layout->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    layout->scale.gravity = ASPECT;
    combobox_add_entry(layout,"qwertz");
    combobox_add_entry(layout,"qwerty");
    combobox_add_entry(layout,"azerty");
    combobox_set_active_entry(layout, 0);
    set_adjustment(layout->adj,0.0, 0.0, 0.0, 2.0, 1.0, CL_ENUM);
    layout->func.value_changed_callback = layout_callback;
    layout->func.key_press_callback = key_press;
    layout->func.key_release_callback = key_release;
    tmp = layout->childlist->childs[0];
    tmp->func.key_press_callback = key_press;
    tmp->func.key_release_callback = key_release;


    keymap = add_hslider(win, "Octave mapping", 540, 2, 150, 32);
    keymap->data = KEYMAP;
    keymap->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    keymap->scale.gravity = ASPECT;
    set_adjustment(keymap->adj,2.0, 2.0, 0.0, 4.0, 1.0, CL_CONTINUOS);
    adj_set_scale(keymap->adj, 0.05);
    keymap->func.value_changed_callback = octave_callback;
    keymap->func.key_press_callback = key_press;
    keymap->func.key_release_callback = key_release;

    w[0] = add_keyboard_knob(win, "PitchBend", 5, 40, 60, 75);
    w[0]->data = PITCHBEND;
    w[0]->func.value_changed_callback = pitchwheel_callback;
    
    w[9] = add_keyboard_knob(win, "Balance", 65, 40, 60, 75);
    w[9]->data = BALANCE;
    w[9]->func.value_changed_callback = balance_callback;

    w[1] = add_keyboard_knob(win, "ModWheel", 125, 40, 60, 75);
    w[1]->data = MODULATION;
    w[1]->func.value_changed_callback = modwheel_callback;

    w[2] = add_keyboard_knob(win, "Detune", 185, 40, 60, 75);
    w[2]->data = CELESTE;
    w[2]->func.value_changed_callback = detune_callback;

    w[10] = add_keyboard_knob(win, "Expression", 245, 40, 60, 75);
    w[10]->data = EXPRESSION;
    w[10]->func.value_changed_callback = expression_callback;

    w[3] = add_keyboard_knob(win, "Attack", 305, 40, 60, 75);
    w[3]->data = ATTACK_TIME;
    w[3]->func.value_changed_callback = attack_callback;

    w[4] = add_keyboard_knob(win, "Release", 365, 40, 60, 75);
    w[4]->data = RELEASE_TIME;
    w[4]->func.value_changed_callback = release_callback;

    w[5] = add_keyboard_knob(win, "Volume", 425, 40, 60, 75);
    w[5]->data = VOLUME;
    w[5]->func.value_changed_callback = volume_callback;

    w[6] = add_keyboard_knob(win, "Velocity", 485, 40, 60, 75);
    w[6]->data = VELOCITY;
    set_adjustment(w[6]->adj,127.0, 127.0, 0.0, 127.0, 1.0, CL_CONTINUOS);
    w[6]->func.value_changed_callback = velocity_callback;

    w[7] = add_toggle_button(win, "Sustain", 550, 45, 75, 30);
    w[7]->data = SUSTAIN;
    w[7]->scale.gravity = ASPECT;
    w[7]->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    w[7]->func.value_changed_callback = sustain_callback;
    w[7]->func.key_press_callback = key_press;
    w[7]->func.key_release_callback = key_release;

    w[8] = add_toggle_button(win, "Sostenuto", 550, 80, 75, 30);
    w[8]->data = SOSTENUTO;
    w[8]->scale.gravity = ASPECT;
    w[8]->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    w[8]->func.value_changed_callback = sostenuto_callback;
    w[8]->func.key_press_callback = key_press;
    w[8]->func.key_release_callback = key_release;

    record = add_toggle_button(win, "_Record", 635, 45, 55, 30);
    record->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    record->func.value_changed_callback = record_callback;
    record->func.key_press_callback = key_press;
    record->func.key_release_callback = key_release;

    play = add_toggle_button(win, "_Play", 635, 80, 55, 30);
    play->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    play->func.value_changed_callback = play_callback;
    play->func.key_press_callback = key_press;
    play->func.key_release_callback = key_release;

    // open a widget for the keyboard layout
    wid = create_widget(app, win, 0, 120, 700, 120);
    wid->flags &= ~USE_TRANSPARENCY;
    wid->flags |= NO_AUTOREPEAT | NO_PROPAGATE;
    wid->scale.gravity = NORTHEAST;
    add_midi_keyboard(wid, "MidiKeyBoard", 0, 0, 700, 120);

    MidiKeyboard *keys = (MidiKeyboard*)wid->parent_struct;

    keys->mk_send_note = get_note;
    keys->mk_send_all_sound_off = get_all_notes_off;

    // when no config file exsist, center window on sreen
    if (!has_config) {
        Screen *screen = DefaultScreenOfDisplay(win->app->dpy);
        main_x = screen->width/2 - main_w/2;
        main_y = screen->height/2 - main_h/2; 
    }

    // set controllers to saved values
    combobox_set_active_entry(channel, mchannel);
    combobox_set_active_entry(layout, keylayout);
    adj_set_value(w[6]->adj, velocity);

    // set window to saved size
    XResizeWindow (win->app->dpy, win->widget, main_w, main_h);

    // start the timeout thread for keyboard animation
    animidi->start(30, std::bind(animate_midi_keyboard,(void*)wid));
}

void dummy_callback(void *w_, void* user_data) {
}

// static
void XKeyBoard::animate_midi_keyboard(void *w_) {
    Widget_t *w = (Widget_t*)w_;
    MidiKeyboard *keys = (MidiKeyboard*)w->parent_struct;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    if (xjmkb->xjack->transport_state != xjmkb->xjack->old_transport_state) {
        xjmkb->xjack->old_transport_state = xjmkb->xjack->transport_state;
        XLockDisplay(w->app->dpy);
        xjmkb->play->func.adj_callback = dummy_callback;
        adj_set_value(xjmkb->play->adj,(float)xjmkb->xjack->transport_state);
        expose_widget(xjmkb->play);
        XFlush(w->app->dpy);
        xjmkb->play->func.adj_callback = transparent_draw;
        XUnlockDisplay(w->app->dpy);
    }

    if ((need_redraw(keys) || xjmkb->run_one_more) && xjmkb->xjack->client) {
        XLockDisplay(w->app->dpy);
        expose_widget(w);
        XFlush(w->app->dpy);
        XUnlockDisplay(w->app->dpy);
        if (xjmkb->run_one_more == 0)
            xjmkb->run_one_more = 40;
    }
    xjmkb->run_one_more = max(0,xjmkb->run_one_more-1);
}

// static
void XKeyBoard::win_configure_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    XWindowAttributes attrs;
    XGetWindowAttributes(w->app->dpy, (Window)w->widget, &attrs);
    if (attrs.map_state != IsViewable) return;
    int x1, y1;
    Window child;
    XTranslateCoordinates( win->app->dpy, win->widget, DefaultRootWindow(
                    win->app->dpy), 0, 0, &x1, &y1, &child );

    xjmkb->main_x = x1;
    xjmkb->main_y = y1;
    xjmkb->main_w = attrs.width;
    xjmkb->main_h = attrs.height;
}

// static
void XKeyBoard::map_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->visible = 1;
}

// static
void XKeyBoard::unmap_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->visible = 0;
}

// static
void XKeyBoard::get_note(Widget_t *w, const int *key, const bool on_off) {
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    if (on_off) {
        xjmkb->mmessage->send_midi_cc(0x90, (*key),xjmkb->velocity, 3);
    } else {
        xjmkb->mmessage->send_midi_cc(0x80, (*key),xjmkb->velocity, 3);
    }
}

// static
void XKeyBoard::get_all_notes_off(Widget_t *w, const int *value) {
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->mmessage->send_midi_cc(0xB0, 123, 0, 3);
}

// static
void XKeyBoard::channel_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->mmessage->channel = xjmkb->mchannel = (int)adj_get_value(w->adj);
    if (xjmkb->xjack->play>0) {
        MidiKeyboard *keys = (MidiKeyboard*)xjmkb->wid->parent_struct;
        clear_key_matrix(keys->in_key_matrix);
    }
}

// static
void XKeyBoard::bank_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->mbank = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 32, xjmkb->mbank, 3);
    xjmkb->mmessage->send_midi_cc(0xC0, xjmkb->mprogram, 0, 2);
}

// static
void XKeyBoard::program_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->mprogram = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 32, xjmkb->mbank, 3);
    xjmkb->mmessage->send_midi_cc(0xC0, xjmkb->mprogram, 0, 2);
}


// static
void XKeyBoard::modwheel_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 1, value, 3);
}

// static
void XKeyBoard::detune_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 94, value, 3);
}

// static
void XKeyBoard::attack_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 73, value, 3);
}

// static
void XKeyBoard::expression_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 11, value, 3);
}

// static 
void XKeyBoard::release_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 72, value, 3);
}

// static 
void XKeyBoard::volume_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 39, value, 3);
}

// static 
void XKeyBoard::velocity_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->velocity = value; 
}

// static
void XKeyBoard::pitchwheel_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    unsigned int change = (unsigned int)(128 * value);
    unsigned int low = change & 0x7f;  // Low 7 bits
    unsigned int high = (change >> 7) & 0x7f;  // High 7 bits
    xjmkb->mmessage->send_midi_cc(0xE0,  low, high, 3);
}

// static
void XKeyBoard::balance_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 8, value, 3);
}

// static
void XKeyBoard::sustain_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 64, value*127, 3);
}

// static
void XKeyBoard::sostenuto_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->mmessage->send_midi_cc(0xB0, 66, value*127, 3);
}

// static
void XKeyBoard::record_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->xjack->record = value;
    if (value > 0) {
        adj_set_value(xjmkb->play->adj,0.0);
        xjmkb->xjack->store1.clear();
        xjmkb->xjack->store2.clear();
        xjmkb->xjack->rec.play.clear();
        xjmkb->xjack->fresh_take = true;
        xjmkb->xjack->first_play = true;
        xjmkb->xjack->rec.start();
    } else if ( xjmkb->xjack->rec.is_running()) {
        if (xjmkb->xjack->rec.st == &xjmkb->xjack->store1 && xjmkb->xjack->store2.size()) {
            xjmkb->xjack->rec.st = &xjmkb->xjack->store2;
        } else {
            xjmkb->xjack->rec.st = &xjmkb->xjack->store1;
        }
        xjmkb->xjack->rec.stop();
    }
}

// static
void XKeyBoard::play_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    int value = (int)adj_get_value(w->adj);
    xjmkb->xjack->play = value;
    if (value < 1) {
        MidiKeyboard *keys = (MidiKeyboard*)xjmkb->wid->parent_struct;
        clear_key_matrix(keys->in_key_matrix);
        xjmkb->mmessage->send_midi_cc(0xB0, 123, 0, 3);
        xjmkb->xjack->first_play = true;
    } else {
        adj_set_value(xjmkb->record->adj,0.0);
    }
}

// static
void XKeyBoard::layout_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    MidiKeyboard *keys = (MidiKeyboard*)xjmkb->wid->parent_struct;
    keys->layout = xjmkb->keylayout = (int)adj_get_value(w->adj);
}

// static
void XKeyBoard::octave_callback(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    MidiKeyboard *keys = (MidiKeyboard*)xjmkb->wid->parent_struct;
    keys->octave = (int)12*adj_get_value(w->adj);
    expose_widget(xjmkb->wid);
}

// static
void XKeyBoard::key_press(void *w_, void *key_, void *user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    XKeyEvent *key = (XKeyEvent*)key_;
    if ((key->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask)) == (ControlMask)) {
        KeySym sym = XLookupKeysym (key, 0);
        if (sym == 112) {
            int value = (int)adj_get_value(xjmkb->play->adj);
            if (value) adj_set_value(xjmkb->play->adj,0.0);
            else adj_set_value(xjmkb->play->adj,1.0);
        } else if (sym == 114) {
            int value = (int)adj_get_value(xjmkb->record->adj);
            if (value) adj_set_value(xjmkb->record->adj,0.0);
            else adj_set_value(xjmkb->record->adj,1.0);
        } else if (sym == 99) {
            xjmkb->signal_handle (2, xjmkb);
        }
    } else {
        xjmkb->wid->func.key_press_callback(xjmkb->wid, key_, user_data);
    }
}

// static
void XKeyBoard::key_release(void *w_, void *key_, void *user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    xjmkb->wid->func.key_release_callback(xjmkb->wid, key_, user_data);
}


// static
void XKeyBoard::signal_handle (int sig, XKeyBoard *xjmkb) {
    if(xjmkb->xjack->client) jack_client_close (xjmkb->xjack->client);
    xjmkb->xjack->client = NULL;
    XLockDisplay(xjmkb->win->app->dpy);
    quit(xjmkb->win);
    XFlush(xjmkb->win->app->dpy);
    XUnlockDisplay(xjmkb->win->app->dpy);
    fprintf (stderr, "\n%s: signal %i received, bye bye ...\n",xjmkb->client_name.c_str(), sig);
}

// static
void XKeyBoard::exit_handle (int sig, XKeyBoard *xjmkb) {
    if(xjmkb->xjack->client) jack_client_close (xjmkb->xjack->client);
    xjmkb->xjack->client = NULL;
    fprintf (stderr, "\n%s: signal %i received, exiting ...\n",xjmkb->client_name.c_str(), sig);
    exit (0);
}


// static
void XKeyBoard::win_mem_free(void *w_, void* user_data) {
    Widget_t *w = (Widget_t*)w_;
    Widget_t *win = get_toplevel_widget(w->app);
    XKeyBoard *xjmkb = (XKeyBoard*) win->parent_struct;
    if(xjmkb->icon) {
        XFreePixmap(xjmkb->win->app->dpy, *xjmkb->icon);
        xjmkb->icon = NULL;
    }
}

/****************************************************************
 ** class PosixSignalHandler
 **
 ** Watch for incomming system signals in a extra thread
 ** 
 */

PosixSignalHandler::PosixSignalHandler(XKeyBoard *xjmkb_)
    : waitset(),
      thread(nullptr),
      xjmkb(xjmkb_),
      exit(false) {
    sigemptyset(&waitset);

    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGQUIT);
    sigaddset(&waitset, SIGTERM);
    sigaddset(&waitset, SIGHUP);
    sigaddset(&waitset, SIGKILL);

    sigprocmask(SIG_BLOCK, &waitset, NULL);
    create_thread();
}

PosixSignalHandler::~PosixSignalHandler() {
    if (thread) {
        exit = true;
        pthread_kill(thread->native_handle(), SIGINT);
        thread->join();
        delete thread;
    }
    sigprocmask(SIG_UNBLOCK, &waitset, NULL);
}

void PosixSignalHandler::create_thread() {
    try {
        thread = new std::thread(
            sigc::mem_fun(*this, &PosixSignalHandler::signal_helper_thread));
    } catch (std::system_error& e) {
        fprintf(stderr,"Thread create failed (signal): %s", e.what());
    }
}

void PosixSignalHandler::signal_helper_thread() {

    pthread_sigmask(SIG_BLOCK, &waitset, NULL);
    while (true) {
        int sig;
        int ret = sigwait(&waitset, &sig);
        if (exit) {
            break;
        }
        if (ret != 0) {
            assert(errno == EINTR);
            continue;
        }
        switch (sig) {
        case SIGINT:
        case SIGTERM:
        case SIGQUIT:
            xjmkb->signal_handle (sig, xjmkb);
            break;
        case SIGHUP:
        case SIGKILL:
            xjmkb->exit_handle (sig, xjmkb);
            break;
        default:
            assert(false);
        }
    }
}

} // namespace midikeyboard


/****************************************************************
 ** main
 **
 ** init the classes, create the UI, open jackd-client and start application
 ** 
 */

int main (int argc, char *argv[]) {
    if(0 == XInitThreads()) 
        fprintf(stderr, "Warning: XInitThreads() failed\n");
    Xputty app;

    midikeyboard::MidiMessenger mmessage;
    nsmhandler::NsmSignalHandler nsmsig;
    midikeyboard::AnimatedKeyBoard  animidi;
    midikeyboard::XJack xjack(&mmessage);
    midikeyboard::XKeyBoard xjmkb(&xjack, &mmessage, nsmsig, &animidi);
    midikeyboard::PosixSignalHandler xsig(&xjmkb);
    nsmhandler::NsmHandler nsmh(&nsmsig);

    nsmsig.nsm_session_control = nsmh.check_nsm(xjmkb.client_name.c_str(), argv);

    xjmkb.read_config();

    main_init(&app);
    
    xjmkb.init_ui(&app);
    xjack.init_jack();

    xjmkb.show_ui(xjmkb.visible);

    main_run(&app);
    
    animidi.stop();
    main_quit(&app);

    if(!nsmsig.nsm_session_control) xjmkb.save_config();

    if (xjack.client) jack_client_close (xjack.client);

    exit (0);

} // namespace midikeyboard

