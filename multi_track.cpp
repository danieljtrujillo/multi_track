/**
	@file
	multi_track

	Max/MSP external that reads audio stems directly from a multichannel buffer~,
	sends them to a Python neural-network server over OSC/UDP, and writes the
	predicted stems back into the same buffer.

	Workflow:
	  1. "set_command <cmd>"        — store the server launch command (local or SSH).
	  2. "server 1/0"               — start / stop the Python server process.
	  3. "set_buffer <name>"        — bind to a multichannel buffer~ (N channels = N stems).
	  4. "load_model"               — push current parameters to the server and load the model.
	  5. "predict <curr>"           — read context from buffer at cursor position,
	                                  send to server as OSC chunks, wait for predictions,
	                                  and write them back into the buffer directly.

	Buffer coordinate system:
	  T           — context window size in samples (set via "T <n>")
	  p           — predict window as fraction of T (set via "percentage <f>")
	  w           — pr_win_mul: -1, 0, or 1 (set via "pr_win_mul <n>")
	  curr        — cursor position passed with each "predict" message
	  fade        — fade-in length in samples applied on write-back (set via "fade <n>")
	  Read from:  curr → curr + T                        (full T-sample context, fixed)
	  Write to:   curr + w*p*T → curr + (w+1)*p*T       (p*T-sample prediction window, with fade-in)

	Send modes (set via "send_mode 0|1"):
	  0 = sum     — sum all channels except target into one context plane (fast, 1/N data)
	  1 = separate — send each non-target channel as its own plane (flexible, full data)

	Inlets:  1 (message)
	Outlets: 1 bang — fires when prediction has been written to buffer

	Tornike Karchkhadze, tkarchkhadze@ucsd.edu
*/




#include "ext.h"
#include "ext_obex.h"

#include <time.h>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <vector>
#include <stdio.h>
#include <thread>
#include <chrono>

#ifdef _WIN32
#define NOMINMAX  // prevent Windows min/max macro conflicts
#include "shellapi.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")  // Link Winsock library
#include <tlhelp32.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
// macOS / POSIX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <sys/stat.h>
#endif

#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include <ip/IpEndpointName.h>
#include "osc/OscReceivedElements.h"
#include "osc/OscPacketListener.h"

#include "z_dsp.h"
#include "ext_buffer.h"

#define OUTPUT_BUFFER_SIZE 65536    // Max OSC send buffer (bytes)
#define MAX_OSC_PACKET_SIZE 65535   // Max UDP receive buffer (bytes)

// Mutex + condition variable wrapper used to synchronise threads:
// server startup, data import acknowledgement, and output completion.
class thread_control
{
	std::mutex mutex;
	std::condition_variable condVar;

public:
	thread_control()
	{}
	void notify()
	{
		condVar.notify_one();
	}
	void waitforit()
	{
		std::unique_lock<std::mutex> mlock(mutex);
		condVar.wait(mlock);
	}
	void lock() {
		mutex.lock();
	}
	void unlock() {
		mutex.unlock();
	}
};



typedef struct _multi_track {
	t_object ob;

	// Model parameters sent to the Python server on load_model
	double percentage;
	double pr_win_mul;

	int verbose_flag;  // 1 = print timing and debug info to Max console
	int batch_id;      // Incremented on each predict call so server can detect new batches

	// OSC listener thread handle
	t_systhread listener_thread;

	// UDP port numbers: MAX → server (SENDER) and server → MAX (LISTENER)
	int PORT_SENDER;
	int PORT_LISTENER;

	// Thread synchronisation
	thread_control* out_tread_control;       // Signals that prediction write-back is complete
	thread_control* server_control;          // Signals that server is ready
	thread_control* python_import_control;   // Signals data import acknowledgement

	// Single bang outlet: fires when prediction has been written to buffer
	void* done_outlet;

	int package_size;  // Number of floats per OSC chunk sent to server

#ifdef _WIN32
	HANDLE server_process = NULL;
	HANDLE server_job;
#else
	pid_t server_pid = 0;
	pid_t server_pgid = 0;
	int terminal_window_id = 0;  // Terminal.app window ID for clean shutdown
	int server_osc_port = 7000;  // Used to kill server by port on stop
#endif

	char command_str[1024];       // Shell command used to start the server
	char client_ip[512] = { 0 }; // Public IP of this machine (sent to server as --client_ip)
	char server_ip[512] = { 0 }; // Resolved IP of the server (used for OSC send)

	int predict_flags[4];  // Per-stem: 1 = model predicts this stem, 0 = pass through
	int live_mode;         // 0 = skip predict at curr=0, 1 = cycle (read tail of buffer)

	// Buffer integration
	t_buffer_ref* buffer_ref;   // Reference to the bound multichannel buffer~
	t_symbol*     buffer_name;  // Name of the bound buffer~
	int           num_stems;    // Number of stems = channel count of buffer (default 4)
	int           send_mode;    // 0 = send summed context, 1 = send channels separately
	long          T_samples;    // Context window size in samples
	double        fade_ratio;   // Crossfade overlap as fraction of sampling rate (e.g. 0.02 = ~882 samples at 44100)
	long          curr;         // Current cursor position (set with each predict call)

	// High-resolution timestamps for end-to-end timing measurements
	std::chrono::high_resolution_clock::time_point packet_test_start_time;
	std::chrono::high_resolution_clock::time_point data_import_start_time;
	std::chrono::high_resolution_clock::time_point data_import_end_time;

	bool auto_load_model_on_ready;  // If true, call load_model as soon as /ready is received

} t_multi_track;


// Prototypes
t_multi_track* multi_track_new(t_symbol* s, long argc, t_atom* argv);
void		multi_track_assist(t_multi_track* x, void* b, long m, long a, char* s);
void		multi_track_free(t_multi_track* x);

void		multi_track_load_model(t_multi_track* x);
void		multi_track_server(t_multi_track* x, long command);
void		multi_track_verbose(t_multi_track* x, long command);
void		multi_track_set_percentage(t_multi_track* x, double percentage);
void		multi_track_set_pr_win_mul(t_multi_track* x, double pr_win_mul);

// Buffer integration
void		multi_track_set_buffer(t_multi_track* x, t_symbol* s);
void		multi_track_set_send_mode(t_multi_track* x, long mode);
void		multi_track_set_T(t_multi_track* x, long T_samples);
void		multi_track_set_fade(t_multi_track* x, double fade_ratio);

void		multi_track_set_live_mode(t_multi_track* x, long mode);

// Main predict entry point: reads buffer, sends to server
void		multi_track_predict(t_multi_track* x, long curr);

void		multi_track_OSC_load_model(t_multi_track* x);

void		send_float_plane(float* data, int num_samples, const char* address, int port, const char* tag, long package_size, int total_expected_chunks, int batch_id);

void*		multi_track_OSC_listener(t_multi_track* x, int argc, char* argv[]);
void		multi_track_OSC_listen_thread(t_multi_track* x);

void		multi_track_set_packet_size(t_multi_track* x, long new_size);
void		multi_track_test_packet(t_multi_track* x);
void		multi_track_send_print(t_multi_track* x);
void		multi_track_send_reset(t_multi_track* x);

void		multi_track_set_command(t_multi_track* x, t_symbol* s, long argc, t_atom* argv);
void		get_public_ip(char* ip_buffer, size_t buffer_size);
void		multi_track_get_client_ip(t_multi_track* x);

void		multi_track_set_predict_instruments(t_multi_track* x, t_symbol* s, long argc, t_atom* argv);
void		multi_track_send_predict_instruments(t_multi_track* x);






// Globals and Statics
static t_class* s_multi_track_class = NULL;

///**********************************************************************/

// Class Definition and Life Cycle

void ext_main(void* r)
{
	t_class* c = class_new("multi_track",
		(method)multi_track_new, (method)multi_track_free,
		sizeof(t_multi_track), (method)NULL, A_GIMME, 0L);

	class_addmethod(c, (method)multi_track_assist,              "assist",              A_CANT,  0);

	// Server lifecycle
	class_addmethod(c, (method)multi_track_set_command,         "set_command",         A_GIMME, 0);
	class_addmethod(c, (method)multi_track_server,              "server",              A_LONG,  0);
	class_addmethod(c, (method)multi_track_load_model,          "load_model",          0);

	// Buffer integration
	class_addmethod(c, (method)multi_track_set_buffer,          "set_buffer",          A_SYM,   0);
	class_addmethod(c, (method)multi_track_set_send_mode,       "send_mode",           A_LONG,  0);
	class_addmethod(c, (method)multi_track_set_T,               "T",                   A_LONG,  0);
	class_addmethod(c, (method)multi_track_set_fade,            "fade",                A_FLOAT, 0);
	class_addmethod(c, (method)multi_track_set_live_mode,       "live_mode",           A_LONG,  0);

	// Main predict trigger — reads buffer, sends to server, writes result back
	class_addmethod(c, (method)multi_track_predict,             "predict",             A_LONG,  0);

	// Model parameters (forwarded to Python server immediately)
	class_addmethod(c, (method)multi_track_set_percentage,      "percentage",          A_FLOAT, 0);
	class_addmethod(c, (method)multi_track_set_pr_win_mul,      "pr_win_mul",          A_FLOAT, 0);
	class_addmethod(c, (method)multi_track_set_packet_size,     "packet_size",         A_LONG,  0);
	class_addmethod(c, (method)multi_track_set_predict_instruments, "predict_instruments", A_GIMME, 0);

	// Utility
	class_addmethod(c, (method)multi_track_verbose,             "verbose",             A_LONG,  0);
	class_addmethod(c, (method)multi_track_test_packet,         "test_packet",         0);
	class_addmethod(c, (method)multi_track_send_print,          "print",               0);
	class_addmethod(c, (method)multi_track_send_reset,          "reset",               0);
	class_addmethod(c, (method)multi_track_get_client_ip,       "get_client_ip",       0);

	CLASS_ATTR_LONG(c, "verb", 0, t_multi_track, verbose_flag);

	class_dspinit(c);
	class_register(CLASS_BOX, c);
	s_multi_track_class = c;
}


/***********************************************************************/
/***********************************************************************/
/********************** Initialisation *********************************/
/***********************************************************************/
/***********************************************************************/

t_multi_track* multi_track_new(t_symbol* s, long argc, t_atom* argv)
{
	t_multi_track* x = (t_multi_track*)object_alloc(s_multi_track_class);

	if (x) {
		x->percentage = 0.0;
		x->pr_win_mul = 0.0;

		x->PORT_SENDER   = 7000;
		x->PORT_LISTENER = 8000;

		x->verbose_flag = 0;
		x->batch_id = 0;

		x->out_tread_control     = new thread_control;
		x->server_control        = new thread_control;
		x->python_import_control = new thread_control;

		// Single outlet: bang when prediction has been written to buffer
		x->done_outlet = bangout((t_object*)x);

		// Buffer integration — must be set via messages before predict is called
		x->buffer_ref   = nullptr;  // checked before use
		x->fade_ratio   = 0.0;
		x->curr         = 0;
		x->live_mode    = 0;

		// First argument is the buffer name: multi_track mybuffer
		if (argc > 0 && atom_gettype(argv) == A_SYM)
			multi_track_set_buffer(x, atom_getsym(argv));

		attr_args_process(x, argc, argv);

		multi_track_OSC_listen_thread(x);

		x->package_size = 10240;

		get_public_ip(x->client_ip, sizeof(x->client_ip));
		post("Public IP: %s", x->client_ip);

		for (int i = 0; i < 4; i++)
			x->predict_flags[i] = 0;

		x->auto_load_model_on_ready = false;
	}
	return x;
}


void multi_track_free(t_multi_track* x) {
	post("Freeing multi_track object and stopping processes...");

	/************** Stop Python server if running ****************/
#ifdef _WIN32
	if (x->server_process) {
		DWORD exit_code;
		if (GetExitCodeProcess(x->server_process, &exit_code) && exit_code == STILL_ACTIVE) {
			post("Stopping Python server...");

			// Terminate the entire process tree if managed by a job object
			if (x->server_job) {
				TerminateJobObject(x->server_job, 0);
				CloseHandle(x->server_job);
				x->server_job = NULL;
			}

			// Terminate the server process
			TerminateProcess(x->server_process, 0);
			CloseHandle(x->server_process);
			x->server_process = NULL;

			post("Python server stopped.");
		}
	}
#else
	if (x->server_pgid > 0) {
		post("Stopping Python server...");
		killpg(x->server_pgid, SIGTERM);
		waitpid(x->server_pid, NULL, WNOHANG);
		x->server_pid = 0;
		x->server_pgid = 0;
		post("Python server stopped.");
	}
#endif

	// Free allocated memory
	if (x->buffer_ref) {
		object_free(x->buffer_ref);
		x->buffer_ref = nullptr;
	}
	delete x->out_tread_control;
	delete x->server_control;
	delete x->python_import_control;

	post("Memory freed. Object cleanup complete.");

}





/**********************************************************************
 * Buffer integration — bind, configure, and predict
 **********************************************************************/

void multi_track_set_buffer(t_multi_track* x, t_symbol* name) {
	if (x->buffer_ref)
		object_free(x->buffer_ref);
	x->buffer_name = name;
	x->buffer_ref  = buffer_ref_new((t_object*)x, name);

	// Read channel count from buffer and use it as num_stems
	t_buffer_obj* buf = buffer_ref_getobject(x->buffer_ref);
	if (buf) {
		x->num_stems = (int)buffer_getchannelcount(buf);
		post("Buffer '%s' bound — %d channels / stems", name->s_name, x->num_stems);
	} else {
		post("Buffer '%s' bound (not yet loaded)", name->s_name);
	}
}

void multi_track_set_send_mode(t_multi_track* x, long mode) {
	if (mode != 0 && mode != 1) { post("send_mode must be 0 (sum) or 1 (separate)"); return; }
	x->send_mode = (int)mode;
	post("send_mode set to %s", mode == 0 ? "sum" : "separate");
}

void multi_track_set_T(t_multi_track* x, long T_samples) {
	if (T_samples <= 0) { post("T must be > 0 samples"); return; }
	x->T_samples = T_samples;
	post("T set to %ld samples", T_samples);
}

void multi_track_set_fade(t_multi_track* x, double ratio) {
	if (ratio < 0.0 || ratio > 1.0) { post("fade ratio must be 0.0 – 1.0"); return; }
	x->fade_ratio = ratio;
	post("fade ratio set to %f", ratio);

	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[64];
	osc::OutboundPacketStream p(buffer, 64);
	p << osc::BeginMessage("/update_fade") << ratio << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}

void multi_track_set_live_mode(t_multi_track* x, long mode) {
	if (mode != 0 && mode != 1) { post("live_mode must be 0 or 1"); return; }
	x->live_mode = (int)mode;
	post("live_mode set to %d (%s)", mode, mode ? "cycling" : "normal");
}



// Main predict entry point.
// Reads context from buffer, sends to server as OSC chunks.
// The server self-triggers inference once all chunks arrive, then sends predictions back.
// The listener writes predictions directly into the buffer and bangs done_outlet.
void multi_track_predict(t_multi_track* x, long curr) {
	if (!x->buffer_ref) {
		object_error((t_object*)x, "No buffer bound. Use 'set_buffer <name>' first.");
		return;
	}

	t_buffer_obj* buf = buffer_ref_getobject(x->buffer_ref);
	if (!buf) {
		object_error((t_object*)x, "Buffer '%s' not found.", x->buffer_name->s_name);
		return;
	}

	// In normal mode, skip at curr=0 (no past context yet).
	// In live_mode, curr=0 means buffer cycled — tail becomes context, so allow it.
	if (curr == 0 && !x->live_mode) return;

	x->curr = curr;
	x->data_import_start_time = std::chrono::high_resolution_clock::now();

	if (x->verbose_flag) {
		// Wall clock timestamp at predict trigger
		auto now_sys = std::chrono::system_clock::now();
		std::time_t now_t = std::chrono::system_clock::to_time_t(now_sys);
		auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(
			now_sys.time_since_epoch()).count() % 1000;
		std::tm tm_buf;
#ifdef _WIN32
		localtime_s(&tm_buf, &now_t);
#else
		localtime_r(&now_t, &tm_buf);
#endif
		post("----------------------------------------");
		post("[PREDICT] curr=%-8ld  %02d:%02d:%02d.%03lld",
			curr, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (long long)ms_part);
	}

	long T    = x->T_samples;
	long step = (long)(T * x->percentage);  // p*T samples per step

	// In live_mode, curr=0 means the buffer just cycled — read the tail as context.
	// In normal mode curr=0 is already guarded above.
	float* samples = buffer_locksamples(buf);
	if (!samples) {
		object_error((t_object*)x, "Failed to lock buffer samples.");
		return;
	}
	long frames   = buffer_getframecount(buf);
	int  channels = (int)buffer_getchannelcount(buf);

	// In live_mode at cycle point (curr=0): read the tail of the buffer as context.
	// Normally: read the step samples immediately before the cursor.
	long read_start, read_end;
	if (curr == 0 && x->live_mode) {
		read_start = frames - step;
		read_end   = frames;
	} else {
		read_start = curr - step;
		read_end   = curr;
	}
	if (read_start < 0) read_start = 0;
	if (read_end > frames) read_end = frames;
	long actual_T = read_end - read_start;

	const char* stem_tags[4] = { "/bass", "/drums", "/guitar", "/piano" };
	int num_stems = (channels < 4) ? channels : 4;

	// Count how many planes we will send (depends on send_mode and predict_flags)
	// In sum mode: one plane per target stem (sum of all other channels)
	// In separate mode: one plane per non-target channel, per target stem
	int num_targets = 0;
	for (int i = 0; i < num_stems; i++)
		if (x->predict_flags[i]) num_targets++;

	int planes_per_target = (x->send_mode == 0) ? 1 : (num_stems - 1);
	int num_planes = num_targets * planes_per_target;
	int chunks_per_plane = ((int)actual_T + (int)x->package_size - 1) / (int)x->package_size;
	int total_expected_chunks = num_planes * chunks_per_plane;
	int batch_id = ++x->batch_id;

	// Build context planes and send — one thread per target stem
	std::vector<std::thread> threads;
	threads.reserve(num_targets);

	for (int t = 0; t < num_stems; t++) {
		if (!x->predict_flags[t]) continue;

		if (x->send_mode == 0) {
			// Sum all channels except target into one context plane
			std::vector<float> context(actual_T, 0.0f);
			for (int c = 0; c < channels && c < 4; c++) {
				if (c == t) continue;
				for (long i = 0; i < actual_T; i++) {
					long frame = read_start + i;
					context[i] += samples[frame * channels + c];
				}
			}
			// Copy so the thread owns the data
			std::vector<float> plane_data = context;
			threads.emplace_back([plane_data, t, &x, stem_tags, total_expected_chunks, batch_id]() mutable {
				send_float_plane(plane_data.data(), (int)plane_data.size(),
					x->server_ip, x->PORT_SENDER, stem_tags[t],
					x->package_size, total_expected_chunks, batch_id);
			});
		} else {
			// Send each non-target channel separately
			for (int c = 0; c < channels && c < 4; c++) {
				if (c == t) continue;
				std::vector<float> plane_data(actual_T);
				for (long i = 0; i < actual_T; i++) {
					long frame = read_start + i;
					plane_data[i] = samples[frame * channels + c];
				}
				threads.emplace_back([plane_data, t, &x, stem_tags, total_expected_chunks, batch_id]() mutable {
					send_float_plane(plane_data.data(), (int)plane_data.size(),
						x->server_ip, x->PORT_SENDER, stem_tags[t],
						x->package_size, total_expected_chunks, batch_id);
				});
			}
		}
	}

	if (x->verbose_flag) {
		double t_send_start = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - x->data_import_start_time).count();
		post("[SEND]    start  +%.1f ms", t_send_start);
	}

	buffer_unlocksamples(buf);

	for (auto& th : threads) th.join();

	x->data_import_end_time = std::chrono::high_resolution_clock::now();

	if (x->verbose_flag) {
		double t_send_done = std::chrono::duration<double, std::milli>(
			x->data_import_end_time - x->data_import_start_time).count();
		auto _now_sys = std::chrono::system_clock::now();
		std::time_t _now_t = std::chrono::system_clock::to_time_t(_now_sys);
		auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(_now_sys.time_since_epoch()).count() % 1000;
		std::tm _tm; localtime_s(&_tm, &_now_t);
		post("[SEND]    done   +%.1f ms  (%d chunks)  %02d:%02d:%02d.%03lld",
			t_send_done, total_expected_chunks, _tm.tm_hour, _tm.tm_min, _tm.tm_sec, (long long)_ms);
	}
	// Server self-triggers inference when it has received all expected chunks.
	// Listener writes predictions back to buffer and bangs done_outlet.
}





/**********************************************************************
 * Parameter setters — each updates the local value and notifies the
 * Python server via OSC so both sides stay in sync.
 **********************************************************************/

void multi_track_set_packet_size(t_multi_track* x, long new_size) {
	if (new_size < 128 || new_size > 16384) {  // Limit range for safety
		post("Invalid packet size. Choose between 128 and 16384 bytes.");
		return;
	}

	x->package_size = new_size;
	post("Packet size set to %d", x->package_size);

	// Send updated package size (chunk size) to Python
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));

	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);

	p << osc::BeginMessage("/update_package_size")
		<< x->package_size  // Send the new package size to Python
		<< osc::EndMessage;

	transmitSocket.Send(p.Data(), p.Size());
	post("Sent OSC message: /update_package_size with size %d", x->package_size);
}

void multi_track_set_percentage(t_multi_track* x, double new_percentage) {
	if (new_percentage < 0.0 || new_percentage > 1.0) {
		post("Invalid percentage. Choose a value between 0.0 and 1.0.");
		return;
	}
	x->percentage = new_percentage;
	post("Percentage set to %f", x->percentage);

	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);
	p << osc::BeginMessage("/update_percentage") << x->percentage << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}

void multi_track_set_pr_win_mul(t_multi_track* x, double new_pr_win_mul) {
	if (new_pr_win_mul < 0.0 || new_pr_win_mul > 2.0) {
		post("Invalid pr_win_mul. Choose a value between 0.0 and 2.0.");
		return;
	}
	x->pr_win_mul = new_pr_win_mul;
	post("pr_win_mul set to %f", x->pr_win_mul);

	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);
	p << osc::BeginMessage("/pr_win_mul") << x->pr_win_mul << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}


void multi_track_test_packet(t_multi_track* x) {
	// Only announce when verbose
	if (x->verbose_flag) {
		post("Starting packet test with size %d (floats)", x->package_size);
	}

	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char osc_buffer[OUTPUT_BUFFER_SIZE];

	// High-resolution timers
	auto t0 = std::chrono::high_resolution_clock::now();

	// Build OSC message
	osc::OutboundPacketStream p(osc_buffer, OUTPUT_BUFFER_SIZE);
	p.Clear();
	p << osc::BeginMessage("/packet_test") << x->package_size;

	// Generate random float data
	for (int i = 0; i < x->package_size; i++) {
		float rand_value = static_cast<float>(rand()) / RAND_MAX;
		p << rand_value;
	}
	p << osc::EndMessage;

	auto t1 = std::chrono::high_resolution_clock::now();

	// Send
	transmitSocket.Send(p.Data(), p.Size());

	auto t2 = std::chrono::high_resolution_clock::now();

	// Store the start time for round-trip measurement
	x->packet_test_start_time = t0;

	if (x->verbose_flag) {
		// Timings
		double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		double send_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

		// Sizes and rates
		const size_t bytes_sent = p.Size();
		const double mb_sent = static_cast<double>(bytes_sent) / (1024.0 * 1024.0);
		const double mbit_sent = static_cast<double>(bytes_sent) * 8.0 / 1e6;
		const double mbps = (send_ms > 0.0) ? (mbit_sent / (send_ms / 1000.0)) : 0.0;

		post("Packet test built in %.3f ms, sent in %.3f ms", build_ms, send_ms);
		post("Bytes sent: %zu (%.3f MB); Effective rate: %.3f Mbit/s", bytes_sent, mb_sent, mbps);
	}
}



// "predict_instruments 1 0 0 0" — which stems the server should predict (1=yes, 0=pass through).
// Context sent is all other channels (summed or separate depending on send_mode).
// Accepts up to 4 arguments; unspecified stems default to 0.
void multi_track_set_predict_instruments(t_multi_track* x, t_symbol* s, long argc, t_atom* argv) {
	if (argc < 1 || argc > 4) {
		post("Error: predict_instruments requires 1–4 arguments, got %ld", argc);
		return;
	}
	for (int i = 0; i < 4; i++)
		x->predict_flags[i] = 0;
	for (int i = 0; i < (int)argc; i++) {
		int value = (int)atom_getlong(&argv[i]);
		if (value != 0 && value != 1) {
			post("Error: argument %d must be 0 or 1, got %d", i, value);
			return;
		}
		x->predict_flags[i] = value;
	}
	post("predict_instruments: bass=%d drums=%d guitar=%d piano=%d",
		x->predict_flags[0], x->predict_flags[1], x->predict_flags[2], x->predict_flags[3]);
	multi_track_send_predict_instruments(x);
}


void multi_track_send_predict_instruments(t_multi_track* x) {
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);
	p << osc::BeginMessage("/predict_instruments")
		<< x->predict_flags[0] << x->predict_flags[1]
		<< x->predict_flags[2] << x->predict_flags[3]
		<< osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}




void multi_track_send_print(t_multi_track* x) {
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);
	p << osc::BeginMessage("/print") << true << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}

void multi_track_send_reset(t_multi_track* x) {
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[256];
	osc::OutboundPacketStream p(buffer, 256);
	p << osc::BeginMessage("/reset") << 1 << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());

	post("Reset");
}


void multi_track_set_command(t_multi_track* x, t_symbol* s, long argc, t_atom* argv) {
	if (argc < 1) {
		post("Usage: set_command <command> [--server_ip <IP>]");
		return;
	}

	// 1) Raw command from Max (quotes are already unescaped here)
	const char* raw = atom_getsym(argv)->s_name;
	strncpy(x->command_str, raw, sizeof(x->command_str) - 1);
	x->command_str[sizeof(x->command_str) - 1] = '\0';

	// 2) Default server_ip; parse from command if present
	strncpy(x->server_ip, "127.0.0.1", sizeof(x->server_ip) - 1);
	x->server_ip[sizeof(x->server_ip) - 1] = '\0';

	auto resolve_to_ip = [](const char* hostname, char* ip_out, size_t ip_out_size) {
		struct addrinfo hints = {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
			inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr, ip_out, (socklen_t)ip_out_size);
			freeaddrinfo(res);
			return true;
		}
		return false;
	};

	if (char* flag = strstr(x->command_str, "--server_ip")) {
		// explicit --server_ip provided: parse and resolve it
		flag += (int)strlen("--server_ip");
		while (*flag == ' ') flag++;
		char* end = flag;
		while (*end && *end != ' ' && *end != '\"') end++;
		size_t len = (size_t)(end - flag);
		if (len > 0 && len < sizeof(x->server_ip)) {
			char hostname[512] = { 0 };
			strncpy(hostname, flag, len);
			hostname[len] = '\0';
			if (!resolve_to_ip(hostname, x->server_ip, sizeof(x->server_ip)))
				strncpy(x->server_ip, hostname, sizeof(x->server_ip) - 1);
			x->server_ip[sizeof(x->server_ip) - 1] = '\0';
		}
	}
	else if (char* ssh = strstr(x->command_str, "ssh ")) {
		// no --server_ip: try to extract hostname from ssh user@host
		char* at = strchr(ssh, '@');
		if (at) {
			at++; // skip '@'
			char* end = at;
			while (*end && *end != ' ' && *end != '\"') end++;
			size_t len = (size_t)(end - at);
			if (len > 0 && len < 512) {
				char hostname[512] = { 0 };
				strncpy(hostname, at, len);
				hostname[len] = '\0';
				if (!resolve_to_ip(hostname, x->server_ip, sizeof(x->server_ip)))
					strncpy(x->server_ip, hostname, sizeof(x->server_ip) - 1);
				x->server_ip[sizeof(x->server_ip) - 1] = '\0';
			}
		}
	}

	// 3) Refresh client IP (your get_public_ip now returns the public IP and logs both)
	get_public_ip(x->client_ip, sizeof(x->client_ip));
	post("Server IP set to: %s", x->server_ip);
	post("Client IP set automatically to: %s", x->client_ip);

	// 4) If --client_ip already present anywhere, keep the command as-is
	if (strstr(x->command_str, "--client_ip") != NULL) {
		post("Command set to: %s", x->command_str);
		return;
	}

	// 5) Find the right injection point and insert --client_ip
	// For local commands:  zsh -ic "... --clientport N"       → inject before last "
	// For SSH commands:    ssh ... "bash -ic '... --clientport N'"  → inject before last '
	// so the arg ends up inside the python command, not outside it.
	char* last_quote = strrchr(x->command_str, '\"');
	if (last_quote) {
		// Look for a single-quote between the second-to-last " and last_quote
		// (i.e. inside the outer double-quoted SSH block, after bash -ic ')
		char* inject_pos = last_quote; // default: before last "
		char* p = last_quote - 1;
		while (p > x->command_str) {
			if (*p == '\'') { inject_pos = p; break; } // found closing ' — inject here
			if (*p == '\"') break; // hit another ", stop looking
			--p;
		}

		// Build injection
		char inject[128];
		snprintf(inject, sizeof(inject), " --client_ip %s",
			x->client_ip[0] ? x->client_ip : "0.0.0.0");
		size_t inject_len = strlen(inject);
		size_t cmd_len = strlen(x->command_str);

		if (cmd_len + inject_len >= sizeof(x->command_str)) {
			post("Warning: command too long to inject --client_ip; skipping injection.");
			post("Command set to: %s", x->command_str);
			return;
		}

		size_t tail_len = cmd_len - (size_t)(inject_pos - x->command_str) + 1;
		memmove(inject_pos + inject_len, inject_pos, tail_len);
		memcpy(inject_pos, inject, inject_len);

		post("Command set to: %s", x->command_str);
		return;
	}

	// 6) Fallback: no double quotes found � append at end
	{
		char inject[128];
		snprintf(inject, sizeof(inject), " --client_ip %s",
			x->client_ip[0] ? x->client_ip : "0.0.0.0");
		if (strlen(x->command_str) + strlen(inject) < sizeof(x->command_str)) {
			strncat(x->command_str, inject, sizeof(x->command_str) - strlen(x->command_str) - 1);
			post("Command set to: %s", x->command_str);
		}
		else {
			post("Warning: command too long to append --client_ip; skipping injection.");
			post("Command set to: %s", x->command_str);
		}
	}
}






void get_public_ip(char* ip_buffer, size_t buffer_size) {
	if (!ip_buffer || buffer_size == 0) return;
	ip_buffer[0] = '\0';

#ifdef _WIN32
	// ---------- 1) Get outbound interface IP (local/NAT-side) ----------
	char outbound_ip[64] = { 0 };
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
			SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
			if (sock != INVALID_SOCKET) {
				sockaddr_in server_addr = {};
				server_addr.sin_family = AF_INET;
				server_addr.sin_port = htons(80);
				inet_pton(AF_INET, "8.8.8.8", &server_addr.sin_addr);  // Google DNS

				if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != SOCKET_ERROR) {
					sockaddr_in local_addr = {};
					int addr_len = sizeof(local_addr);
					if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) == 0) {
						inet_ntop(AF_INET, &local_addr.sin_addr, outbound_ip, sizeof(outbound_ip));
					}
				}
				closesocket(sock);
			}
			WSACleanup();
		}
	}

	// ---------- 2) Fetch true public IP via HTTPS (api.ipify.org) ----------
	char public_ip[64] = { 0 };
	bool got_public = false;
	{
		HINTERNET hSession = WinHttpOpen(L"multi_track/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (hSession) {
			HINTERNET hConnect = WinHttpConnect(hSession, L"api.ipify.org",
				INTERNET_DEFAULT_HTTPS_PORT, 0);
			if (hConnect) {
				HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/",
					NULL, WINHTTP_NO_REFERER,
					WINHTTP_DEFAULT_ACCEPT_TYPES,
					WINHTTP_FLAG_SECURE);
				if (hRequest &&
					WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
						WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
					WinHttpReceiveResponse(hRequest, NULL)) {

					DWORD avail = 0, read = 0;
					std::string body;
					do {
						if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
						std::string chunk(avail, '\0');
						if (!WinHttpReadData(hRequest, &chunk[0], avail, &read) || read == 0) break;
						chunk.resize(read);
						body += chunk;
					} while (avail > 0);

					if (!body.empty()) {
						while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' ' || body.back() == '\t'))
							body.pop_back();
						strncpy(public_ip, body.c_str(), sizeof(public_ip) - 1);
						public_ip[sizeof(public_ip) - 1] = '\0';
						got_public = true;
					}
				}
				if (hRequest) WinHttpCloseHandle(hRequest);
				WinHttpCloseHandle(hConnect);
			}
			WinHttpCloseHandle(hSession);
		}
	}

	// ---------- 3) Log both & choose return value ----------
	post("Outbound interface IP: %s", outbound_ip[0] ? outbound_ip : "(unknown)");
	if (got_public) {
		post("Public (NAT) IP: %s", public_ip);
		strncpy(ip_buffer, public_ip, buffer_size - 1);
		ip_buffer[buffer_size - 1] = '\0';
	}
	else {
		post("Public (NAT) IP: (failed to fetch via HTTPS)");
		if (outbound_ip[0]) {
			strncpy(ip_buffer, outbound_ip, buffer_size - 1);
			ip_buffer[buffer_size - 1] = '\0';
		}
	}

#else
	// ---------- macOS: 1) Get outbound interface IP via connect trick ----------
	char outbound_ip[64] = { 0 };
	{
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock >= 0) {
			struct sockaddr_in server_addr = {};
			server_addr.sin_family = AF_INET;
			server_addr.sin_port = htons(80);
			inet_pton(AF_INET, "8.8.8.8", &server_addr.sin_addr);

			if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
				struct sockaddr_in local_addr = {};
				socklen_t addr_len = sizeof(local_addr);
				if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) == 0) {
					inet_ntop(AF_INET, &local_addr.sin_addr, outbound_ip, sizeof(outbound_ip));
				}
			}
			close(sock);
		}
	}

	// ---------- macOS: 2) Fetch public IP via curl ----------
	char public_ip[64] = { 0 };
	bool got_public = false;
	{
		FILE* fp = popen("curl -s --max-time 5 https://api.ipify.org", "r");
		if (fp) {
			if (fgets(public_ip, sizeof(public_ip), fp)) {
				// trim whitespace
				size_t len = strlen(public_ip);
				while (len > 0 && (public_ip[len-1] == '\r' || public_ip[len-1] == '\n' || public_ip[len-1] == ' '))
					public_ip[--len] = '\0';
				if (len > 0) got_public = true;
			}
			pclose(fp);
		}
	}

	// ---------- 3) Log both & choose return value ----------
	post("Outbound interface IP: %s", outbound_ip[0] ? outbound_ip : "(unknown)");
	if (got_public) {
		post("Public (NAT) IP: %s", public_ip);
		strncpy(ip_buffer, public_ip, buffer_size - 1);
		ip_buffer[buffer_size - 1] = '\0';
	}
	else {
		post("Public (NAT) IP: (failed to fetch via curl)");
		if (outbound_ip[0]) {
			strncpy(ip_buffer, outbound_ip, buffer_size - 1);
			ip_buffer[buffer_size - 1] = '\0';
		}
	}
#endif
}


void multi_track_get_client_ip(t_multi_track* x) {
	get_public_ip(x->client_ip, sizeof(x->client_ip));
	post("Client public IP: %s", x->client_ip);
}




void multi_track_assist(t_multi_track* x, void* b, long m, long a, char* s)
{
	if (m == ASSIST_INLET)
		sprintf(s, "Messages: predict <curr>, set_buffer <name>, T <n>, fade <n>, pr_win_mul <-1|0|1>, send_mode 0|1, ...");
	else
		sprintf(s, "bang — prediction written to buffer");
}

void multi_track_verbose(t_multi_track* x, long command) {
	x->verbose_flag = command;
	if (command == 1) {
		post("Verbose on — listening on port %d, sending to port %d",
			x->PORT_LISTENER, x->PORT_SENDER);
	}
	// Mirror to Python server so its timing prints stay in sync
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[64];
	osc::OutboundPacketStream p(buffer, 64);
	p << osc::BeginMessage("/verbose") << (int)command << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}



// Pushes all current parameters to the server then triggers model load
void multi_track_load_model(t_multi_track* x) {
	post("Loading model...");
	multi_track_set_packet_size(x, x->package_size);
	multi_track_set_percentage(x, x->percentage);
	multi_track_set_pr_win_mul(x, x->pr_win_mul);
	multi_track_set_fade(x, x->fade_ratio);
	multi_track_send_predict_instruments(x);
	multi_track_verbose(x, x->verbose_flag);  // sync verbose state to server
	multi_track_OSC_load_model(x);
	post("Model load request sent.");
}


/**********************************************************************
 * Python Server — start / stop
 **********************************************************************/

#ifdef _WIN32
// Function to terminate a process and its children (Windows only)
void terminate_process_tree(DWORD process_id) {
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) {
		post("Failed to create process snapshot.");
		return;
	}

	PROCESSENTRY32 pe;
	pe.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(hSnapshot, &pe)) {
		do {
			if (pe.th32ParentProcessID == process_id) {
				// Recursively terminate child processes
				terminate_process_tree(pe.th32ProcessID);
			}
		} while (Process32Next(hSnapshot, &pe));
	}

	// Terminate the main process
	HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, process_id);
	if (hProcess != NULL) {
		TerminateProcess(hProcess, 0);
		CloseHandle(hProcess);
	}

	CloseHandle(hSnapshot);
}
#else
// macOS: terminate process group (covers all child processes)
void terminate_process_tree(pid_t pgid) {
	if (pgid > 0)
		killpg(pgid, SIGTERM);
}
#endif


void multi_track_server(t_multi_track* x, long command) {
	if (command == 1) { // Start the server

#ifdef _WIN32
		if (x->server_process != NULL) {
			DWORD exit_code;
			if (GetExitCodeProcess(x->server_process, &exit_code) && exit_code == STILL_ACTIVE) {
				post("Server is already running.");
				return;
			}
		}
#else
		if (x->server_pid > 0 && kill(x->server_pid, 0) == 0) {
			post("Server is already running.");
			return;
		}
#endif

		if (x->command_str[0] == '\0') {
			post("Error: No command defined. Use 'set_command' to define the server command.");
			return;
		}

#ifdef _WIN32
		char command_str[512];
		snprintf(command_str, sizeof(command_str), "cmd.exe /C %s", x->command_str);

		STARTUPINFO si;
		PROCESS_INFORMATION pi;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		ZeroMemory(&pi, sizeof(pi));

		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_SHOWNORMAL;

		HANDLE hJob = CreateJobObject(NULL, NULL);
		if (hJob == NULL) {
			post("Failed to create job object.");
			return;
		}

		if (!CreateProcess(NULL, command_str, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
			post("Failed to start the server.");
			CloseHandle(hJob);
			return;
		}

		if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
			post("Failed to assign process to job object.");
			CloseHandle(hJob);
			TerminateProcess(pi.hProcess, 0);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return;
		}

		x->server_process = pi.hProcess;
		x->server_job = hJob;
		CloseHandle(pi.hThread);

#else
		// macOS: write command to a temp shell script, then use osascript to
		// open it in Terminal.app and capture the window ID for clean shutdown.

		// Parse --serverport from command so stop can kill by port
		x->server_osc_port = 7000; // default
		if (char* pf = strstr(x->command_str, "--serverport")) {
			pf += strlen("--serverport");
			while (*pf == ' ') pf++;
			x->server_osc_port = atoi(pf);
		}

		// Write the server command to a temp shell script
		const char* sh_path = "/tmp/multi_track_server.sh";
		FILE* fp = fopen(sh_path, "w");
		if (fp) {
			fprintf(fp, "#!/bin/zsh\n%s\n", x->command_str);
			fclose(fp);
			chmod(sh_path, 0755);
		} else {
			post("Failed to write server launch script.");
			return;
		}

		// Write AppleScript to open the script in Terminal and return window ID
		const char* scpt_path = "/tmp/multi_track_open.scpt";
		FILE* scpt = fopen(scpt_path, "w");
		if (scpt) {
			fprintf(scpt, "tell application \"Terminal\"\n");
			fprintf(scpt, "  set newTab to do script \"%s\"\n", sh_path);
			fprintf(scpt, "  activate\n");
			fprintf(scpt, "  set winID to id of window 1\n");
			fprintf(scpt, "end tell\n");
			fprintf(scpt, "return winID\n");
			fclose(scpt);
		} else {
			post("Failed to write AppleScript.");
			return;
		}

		// Run osascript and capture the Terminal window ID
		char popen_cmd[256];
		snprintf(popen_cmd, sizeof(popen_cmd), "osascript %s", scpt_path);
		FILE* pipe = popen(popen_cmd, "r");
		if (pipe) {
			int win_id = 0;
			fscanf(pipe, "%d", &win_id);
			pclose(pipe);
			x->terminal_window_id = win_id;
			post("Server terminal window ID: %d", win_id);
		} else {
			post("Failed to launch Terminal via osascript.");
			return;
		}
#endif

		x->auto_load_model_on_ready = true;
		post("Server started. Waiting for server ready signal...");
	}
	else if (command == 0) { // Stop the server

#ifdef _WIN32
		if (x->server_process) {
			DWORD exit_code;
			if (GetExitCodeProcess(x->server_process, &exit_code)) {
				if (exit_code == STILL_ACTIVE) {
					post("Stopping the server...");
					if (x->server_job != NULL) {
						if (!TerminateJobObject(x->server_job, 0))
							post("Failed to terminate job object.");
						CloseHandle(x->server_job);
						x->server_job = NULL;
					}
					CloseHandle(x->server_process);
					x->server_process = NULL;
					post("Server stopped.");
				}
				else {
					post("Server was already stopped.");
					x->server_process = NULL;
				}
			}
			else {
				post("Failed to get exit code of the server process.");
			}
		}
		else {
			post("No active server process found.");
		}
#else
		post("Stopping the server...");
		// 1. Kill server process by port (reliable regardless of process hierarchy)
		char kill_cmd[256];
		snprintf(kill_cmd, sizeof(kill_cmd),
			"lsof -ti :%d | xargs kill -TERM 2>/dev/null; "
			"sleep 0.5; "
			"lsof -ti :%d | xargs kill -KILL 2>/dev/null; true",
			x->server_osc_port, x->server_osc_port);
		system(kill_cmd);

		// 2. Close the Terminal window by ID — no running process so no prompt
		if (x->terminal_window_id != 0) {
			char close_cmd[256];
			snprintf(close_cmd, sizeof(close_cmd),
				"osascript -e 'tell application \"Terminal\" to close (windows whose id is %d)'",
				x->terminal_window_id);
			system(close_cmd);
			x->terminal_window_id = 0;
		}
		x->server_pid = 0;
		x->server_pgid = 0;
		post("Server stopped.");
#endif
	}
}


/**********************************************************************
 * Data Send — called from per-stem threads during predict
 **********************************************************************/

// Sends a float array to the Python server as OSC chunks.
// Each chunk carries: batch_id, start_index, total_expected_chunks, and float values.
// Called concurrently from separate threads (one per active plane).
void send_float_plane(float* data, int num_samples, const char* address, int port, const char* tag, long package_size, int total_expected_chunks, int batch_id) {
	UdpTransmitSocket transmitSocket(IpEndpointName(address, port));
	char osc_buffer[OUTPUT_BUFFER_SIZE];

	int num_chunks = (num_samples + package_size - 1) / package_size;

	for (int chunk = 0; chunk < num_chunks; chunk++) {
		osc::OutboundPacketStream p(osc_buffer, OUTPUT_BUFFER_SIZE);
		p.Clear();
		p << osc::BeginMessage(tag);

		int start_index = chunk * package_size;
		int end_index   = (start_index + (int)package_size < num_samples) ? start_index + (int)package_size : num_samples;
		p << batch_id;
		p << start_index;
		p << total_expected_chunks;

		for (int i = start_index; i < end_index; i++)
			p << data[i];

		p << osc::EndMessage;
		transmitSocket.Send(p.Data(), p.Size());
	}
}



void multi_track_OSC_load_model(t_multi_track* x) {
	UdpTransmitSocket transmitSocket(IpEndpointName(x->server_ip, x->PORT_SENDER));
	char buffer[64];
	osc::OutboundPacketStream p(buffer, 64);
	p << osc::BeginMessage("/load_model") << osc::EndMessage;
	transmitSocket.Send(p.Data(), p.Size());
}






/**********************************************************************
 * OSC Listener — receives predicted stems from the Python server
 **********************************************************************/

class multi_track_packetListener : public osc::OscPacketListener {

public:

	thread_control* out_tread_control;
	thread_control* server_control;
	thread_control* python_import_control;

	// Bang outlet — fires when prediction has been written to buffer
	void* done_outlet;

	// Buffer write-back state (pointers into t_multi_track)
	t_buffer_ref** buffer_ref;
	long* curr;
	long* T_samples;
	double* fade_ratio;
	double* percentage;
	double* pr_win_mul;
	int*    live_mode;

	// Chunk reassembly — buffers incoming chunks per stem until all arrive, then outputs in order
	struct StemReassembly {
		std::vector<std::vector<float>> chunks;
		int expected = 0;
		int received = 0;
		int watchdog_gen = 0;  // incremented on reset; stale watchdogs self-cancel
		std::chrono::high_resolution_clock::time_point first_chunk_time;
	};
	StemReassembly reassembly[4];  // [0]=bass [1]=drums [2]=guitar [3]=piano
	std::mutex reassembly_mutex;

	std::chrono::high_resolution_clock::time_point* packet_test_start_time;
	std::chrono::high_resolution_clock::time_point* data_import_start_time;
	int* verbose_flag;
	bool* auto_load_model_on_ready;
	t_multi_track* multi_track_obj;

	void ProcessBundle(const osc::ReceivedBundle& b, const IpEndpointName& remoteEndpoint)
	{
		(void)remoteEndpoint;

		for (osc::ReceivedBundle::const_iterator i = b.ElementsBegin();
			i != b.ElementsEnd(); ++i) {
			if (i->IsBundle())
				ProcessBundle(osc::ReceivedBundle(*i), remoteEndpoint);
			else
				ProcessMessage(osc::ReceivedMessage(*i), remoteEndpoint);   //, NULL
		}
	}




	virtual void ProcessMessage(const osc::ReceivedMessage& m, const IpEndpointName& remoteEndpoint)
	{
		(void)remoteEndpoint;

		try {
			// Server ready — auto-load model if flag is set
			if (std::strcmp(m.AddressPattern(), "/ready") == 0) {
				osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
				bool server_ready;
				args >> server_ready >> osc::EndMessage;
				if (server_ready) {
					post("Server is ready!");
					if (*auto_load_model_on_ready) {
						post("Auto-loading model...");
						multi_track_load_model(multi_track_obj);
						*auto_load_model_on_ready = false;
					}
					server_control->notify();
				}
			}


			if (std::strcmp(m.AddressPattern(), "/packet_test_response") == 0) {
				double round_trip_ms = std::chrono::duration<double, std::milli>(
					std::chrono::high_resolution_clock::now() - *packet_test_start_time).count();

				osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
				osc::int32 received_size;
				args >> received_size;

				float* received_values = new float[received_size];
				int count = 0;
				float value;
				while (!args.Eos() && count < received_size) {
					args >> value;
					received_values[count++] = value;
				}
				delete[] received_values;

				post("========================================");
				post("Packet test — round-trip: %.3f ms, received %d/%d floats%s",
					round_trip_ms, count, (int)received_size,
					count == received_size ? "" : " — WARNING: count mismatch!");
				post("========================================");
			}

			// Prediction data chunks — reassemble per stem then write to buffer
			int stem_idx = -1;
			if      (std::strcmp(m.AddressPattern(), "/bass")   == 0) stem_idx = 0;
			else if (std::strcmp(m.AddressPattern(), "/drums")  == 0) stem_idx = 1;
			else if (std::strcmp(m.AddressPattern(), "/guitar") == 0) stem_idx = 2;
			else if (std::strcmp(m.AddressPattern(), "/piano")  == 0) stem_idx = 3;
			else return;

			osc::ReceivedMessageArgumentStream args = m.ArgumentStream();
			osc::int32 chunk_idx, total_chunks;
			args >> chunk_idx >> total_chunks;

			// Collect floats from this chunk
			std::vector<float> chunk_data;
			float value;
			while (!args.Eos()) {
				args >> value;
				chunk_data.push_back(value);
			}

			// Store chunk and check if the transfer is complete
			std::vector<std::vector<float>> chunks_to_output;
			bool spawn_watchdog = false;
			double watchdog_timeout_ms = 500.0;  // fallback when only 1 chunk received so far
			int watchdog_gen = -1;
			{
				std::lock_guard<std::mutex> lock(reassembly_mutex);
				auto& r = reassembly[stem_idx];

				// Reset state on first chunk or if total_chunks changed (new transfer)
				if (chunk_idx == 0 || (int)r.chunks.size() != total_chunks) {
					r.chunks.assign(total_chunks, {});
					r.expected = total_chunks;
					r.received = 0;
					r.first_chunk_time = std::chrono::high_resolution_clock::now();
					if (*verbose_flag) {
						double t_rx_first = std::chrono::duration<double, std::milli>(
							r.first_chunk_time - *data_import_start_time).count();
						auto _now_sys = std::chrono::system_clock::now();
						std::time_t _now_t = std::chrono::system_clock::to_time_t(_now_sys);
						auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(_now_sys.time_since_epoch()).count() % 1000;
						std::tm _tm; localtime_s(&_tm, &_now_t);
						post("[RX]      first  +%.1f ms  (%s)  %02d:%02d:%02d.%03lld",
							t_rx_first, m.AddressPattern(), _tm.tm_hour, _tm.tm_min, _tm.tm_sec, (long long)_ms);
					}
				}

				if (chunk_idx < (int)r.chunks.size() && r.chunks[chunk_idx].empty()) {
					r.chunks[chunk_idx] = std::move(chunk_data);
					r.received++;
				}

				if (r.received >= r.expected) {
					// All chunks arrived — move to output
					chunks_to_output = std::move(r.chunks);
					r.chunks.clear();
					r.received = 0;
					if (*verbose_flag) {
						double t_rx_done = std::chrono::duration<double, std::milli>(
							std::chrono::high_resolution_clock::now() - *data_import_start_time).count();
						post("[RX]      complete +%.1f ms  (%s, %d chunks)", t_rx_done, m.AddressPattern(), total_chunks);
					}
				} else {
					// Reset adaptive watchdog on every chunk arrival.
					// Timeout = avg inter-chunk interval × 5 (same formula as Python server).
					r.watchdog_gen++;
					if (r.received > 1) {
						double span_ms = std::chrono::duration<double, std::milli>(
							std::chrono::high_resolution_clock::now() - r.first_chunk_time).count();
						watchdog_timeout_ms = (span_ms / (r.received - 1)) * 5.0;
					}
					watchdog_gen = r.watchdog_gen;
					spawn_watchdog = true;
				}
			}

			// Helper: write assembled chunks into the buffer at the correct position,
			// apply fade-in, then bang done_outlet.
			auto write_to_buffer = [this, stem_idx](std::vector<std::vector<float>>& chunks, bool is_flush) {
				auto t_start = std::chrono::high_resolution_clock::now();
				if (*verbose_flag && is_flush) post("[WRITE]   flush (stem %d, missing chunks filled with zeros)", stem_idx);

				t_buffer_obj* buf = buffer_ref_getobject(*buffer_ref);
				if (!buf) {
					post("WARNING: buffer not available for write-back");
					out_tread_control->notify();
					return;
				}

				float* samples = buffer_locksamples(buf);
				if (!samples) {
					post("WARNING: could not lock buffer samples");
					out_tread_control->notify();
					return;
				}

				long   frames       = buffer_getframecount(buf);
				int    channels     = (int)buffer_getchannelcount(buf);
				// Write position: curr + w*p*T → curr + (w+1)*p*T
				// With crossfade: writing starts 'fade' samples early so the first
				// 'fade' samples blend existing buffer content into the new prediction.
				long   fade          = (long)(*fade_ratio * buffer_getsamplerate(buf));
				long   content_start = *curr + (long)(*pr_win_mul * *percentage * *T_samples);
				long   write_start   = content_start - fade;   // start fade samples before content
				long   write_pos     = write_start;

				for (auto& chunk : chunks) {
					int n = chunk.empty() ? 1024 : (int)chunk.size();
					for (int k = 0; k < n; k++) {
						// In live_mode, wrap write position when it reaches end of buffer
						long wp = (*live_mode && write_pos >= frames) ? write_pos % frames : write_pos;
						if (wp >= 0 && wp < frames) {
							float v = chunk.empty() ? 0.0f : chunk[k];
							// Crossfade: blend existing → new over the first 'fade' samples
							long offset = write_pos - write_start;
							if (fade > 0 && offset < fade) {
								float t = (float)offset / (float)fade;
								float existing = samples[wp * channels + stem_idx];
								v = existing * (1.0f - t) + v * t;
							}
							samples[wp * channels + stem_idx] = v;
						}
						write_pos++;
					}
				}

				buffer_unlocksamples(buf);
				buffer_setdirty(buf);

				post("[WRITE] first_sample=%ld  last_sample=%ld  total=%ld  fade=%ld  content_start=%ld",
					write_start, write_pos - 1, write_pos - write_start, fade, content_start);

				if (*verbose_flag) {
					auto t_now = std::chrono::high_resolution_clock::now();
					double write_ms = std::chrono::duration<double, std::milli>(t_now - t_start).count();
					double total_ms = std::chrono::duration<double, std::milli>(t_now - *data_import_start_time).count();
					auto _now_sys = std::chrono::system_clock::now();
					std::time_t _now_t = std::chrono::system_clock::to_time_t(_now_sys);
					auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(_now_sys.time_since_epoch()).count() % 1000;
					std::tm _tm; localtime_s(&_tm, &_now_t);
					post("[WRITE]   done   +%.1f ms  (write %.1f ms)  total: %.1f ms  %02d:%02d:%02d.%03lld",
						total_ms, write_ms, total_ms, _tm.tm_hour, _tm.tm_min, _tm.tm_sec, (long long)_ms);
					post("----------------------------------------");
				}

				// Bang done outlet and signal output thread
				outlet_bang(done_outlet);
				out_tread_control->notify();
			};

			// Watchdog — fires if remaining chunks don't arrive within the adaptive timeout.
			// Each chunk resets the watchdog via watchdog_gen; stale watchdogs self-cancel.
			if (spawn_watchdog) {
				auto* listener = this;
				std::thread([listener, stem_idx, watchdog_gen, watchdog_timeout_ms, write_to_buffer]() {
					std::this_thread::sleep_for(
						std::chrono::duration<double, std::milli>(watchdog_timeout_ms));
					std::vector<std::vector<float>> flush_chunks;
					{
						std::lock_guard<std::mutex> lock(listener->reassembly_mutex);
						auto& r = listener->reassembly[stem_idx];
						if (r.watchdog_gen != watchdog_gen || r.received == 0) return;
						post("WARNING: watchdog fired — missing %d/%d chunks for stem %d, flushing with zeros",
							r.expected - r.received, r.expected, stem_idx);
						flush_chunks = std::move(r.chunks);
						r.chunks.clear();
						r.received = 0;
					}
					write_to_buffer(flush_chunks, true);
				}).detach();
			}

			// All chunks received — write to buffer in order, then signal done
			if (!chunks_to_output.empty()) {
				write_to_buffer(chunks_to_output, false);
			}

		}
		catch (osc::Exception& e) {
			post("OSC parse error: %s", e.what());
		}
	}

};

// Raw UDP socket that feeds into an OscPacketListener.
// Uses a 4 MB receive buffer to absorb bursts of prediction chunks.
class CustomUdpListener {
private:
#ifdef _WIN32
	SOCKET sock_fd;
#else
	int sock_fd;
#endif
	char buffer[MAX_OSC_PACKET_SIZE];
	osc::OscPacketListener* packetListener;
	sockaddr_in serverAddr;

public:
	CustomUdpListener(int port, osc::OscPacketListener* listener)
		: packetListener(listener) {

#ifdef _WIN32
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
			throw std::runtime_error("WSAStartup failed");
#endif

		sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

#ifdef _WIN32
		if (sock_fd == INVALID_SOCKET) {
			WSACleanup();
			throw std::runtime_error("Socket creation failed");
		}
#else
		if (sock_fd < 0)
			throw std::runtime_error("Socket creation failed");
#endif

		int buffer_size = 4 * 1024 * 1024;  // 4MB — same as Python server, handles burst of all chunks
#ifdef _WIN32
		if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buffer_size, sizeof(buffer_size)) == SOCKET_ERROR) {
			closesocket(sock_fd);
			WSACleanup();
			throw std::runtime_error("Failed to set socket buffer size");
		}
#else
		if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
			close(sock_fd);
			throw std::runtime_error("Failed to set socket buffer size");
		}
#endif

		memset(&serverAddr, 0, sizeof(serverAddr));
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_addr.s_addr = INADDR_ANY;
		serverAddr.sin_port = htons(port);

#ifdef _WIN32
		if (bind(sock_fd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
			closesocket(sock_fd);
			WSACleanup();
			throw std::runtime_error("Failed to bind socket");
		}
#else
		if (bind(sock_fd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
			close(sock_fd);
			throw std::runtime_error("Failed to bind socket");
		}
#endif
	}

	~CustomUdpListener() {
#ifdef _WIN32
		closesocket(sock_fd);
		WSACleanup();
#else
		close(sock_fd);
#endif
	}

	void run() {
		while (true) {
			sockaddr_in clientAddr;
#ifdef _WIN32
			int clientAddrLen = sizeof(clientAddr);
#else
			socklen_t clientAddrLen = sizeof(clientAddr);
#endif
			int receivedBytes = recvfrom(sock_fd, buffer, MAX_OSC_PACKET_SIZE, 0, (struct sockaddr*)&clientAddr, &clientAddrLen);
			if (receivedBytes > 0) {
				try {
					packetListener->ProcessPacket(buffer, receivedBytes, IpEndpointName(clientAddr.sin_addr.s_addr, ntohs(clientAddr.sin_port)));
				}
				catch (const osc::Exception& e) {
					std::cerr << "Error processing OSC packet: " << e.what() << std::endl;
				}
			}
		}
	}
};

void multi_track_OSC_listen_thread(t_multi_track* x) {
	if (x->listener_thread == NULL) {
		systhread_create((method)multi_track_OSC_listener, x, 0, 0, 0, &x->listener_thread);
	}
}


void* multi_track_OSC_listener(t_multi_track* x, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	try {
		multi_track_packetListener listener;

		// Buffer write-back
		listener.done_outlet  = x->done_outlet;
		listener.buffer_ref   = &x->buffer_ref;
		listener.curr         = &x->curr;
		listener.T_samples    = &x->T_samples;
		listener.fade_ratio   = &x->fade_ratio;
		listener.percentage   = &x->percentage;
		listener.pr_win_mul   = &x->pr_win_mul;
		listener.live_mode    = &x->live_mode;

		listener.packet_test_start_time   = &x->packet_test_start_time;
		listener.data_import_start_time   = &x->data_import_start_time;
		listener.verbose_flag             = &x->verbose_flag;
		listener.auto_load_model_on_ready = &x->auto_load_model_on_ready;
		listener.multi_track_obj          = x;

		listener.out_tread_control     = x->out_tread_control;
		listener.server_control        = x->server_control;
		listener.python_import_control = x->python_import_control;

		CustomUdpListener udpListener(x->PORT_LISTENER, &listener);
		udpListener.run();
	}
	catch (const std::exception& e) {
		post("Error starting OSC listener: %s", e.what());
	}

	return NULL;
}

