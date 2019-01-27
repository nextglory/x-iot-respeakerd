#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dbus/dbus.h>
#include <cstring>
#include <memory>
#include <iostream>
#include <sstream>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>

extern "C"
{
#include <sndfile.h>
#include <unistd.h>
#include <getopt.h>
}

#include <respeaker.h>
#include <chain_nodes/pulse_collector_node.h>
#include <chain_nodes/vep_aec_beamforming_node.h>
#include <chain_nodes/snips_1b_doa_kws_node.h>
#include <chain_nodes/snowboy_1b_doa_kws_node.h>


#include "json.hpp"
#include "cppcodec/base64_default_rfc4648.hpp"
#include "INIReader.h"

using namespace respeaker;
using json = nlohmann::json;
using base64 = cppcodec::base64_rfc4648;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<SteadyClock>;

#define CONFIG_FILE    "/etc/respeaker/respeakerd.conf"
#define SOCKET_FILE    "/tmp/respeakerd.sock"
#define BLOCK_SIZE_MS    8
#define RECV_BUFF_LEN    1024
#define STOP_CAPTURE_TIMEOUT    15000    //millisecond
#define WAIT_READY_TIMEOUT      30000    //millisecond
#define SKIP_KWS_TIME_ON_SPEAK  2000     //millisecond

#define MODE_STANDARD           0
#define MODE_PULSE              1
#define MODE_M_WITH_KWS         2
#define MODE_M_WITHOUT_KWS      3

#define KWS_SNIPS               0
#define KWS_SNOWBOY             1
#define KWS_NONE                99

// default value for config vars
std::string default_mode                = "standard";
std::string default_mic_type            = "CIRCULAR_6MIC";

std::string default_hotword_engine      = "snowboy";
std::string default_snowboy_res_path    = "/usr/share/respeaker/snowboy/resources/common.res";
std::string default_snowboy_model_path  = "/usr/share/respeaker/snowboy/resources/snowboy.umdl";
std::string default_snowboy_sensitivity = "0.5";
std::string default_snips_model_path    = "/usr/share/respeaker/snips/model";
double      default_snips_sensitivity   = 0.5;

std::string default_source              = "default";
int32_t     default_agc_level           = -3;
bool        default_debug               = false;
bool        default_enable_wav_log      = false;
int32_t     default_ref_channel         = 6;
std::string default_fifo_file           = "/tmp/music.input";
bool        default_dynamic_doa         = false;

// config vars
std::string config_mode                = default_mode;
std::string config_mic_type            = default_mic_type;
bool        config_test                = false;

std::string config_hotword_engine      = default_hotword_engine;
std::string config_snowboy_res_path    = default_snowboy_res_path;
std::string config_snowboy_model_path  = default_snowboy_model_path;
std::string config_snowboy_sensitivity = default_snowboy_sensitivity;
std::string config_snips_model_path    = default_snips_model_path;
double      config_snips_sensitivity   = default_snips_sensitivity;

std::string config_source              = default_source;
int32_t     config_agc_level           = default_agc_level;
bool        config_debug               = default_debug;
bool        config_enable_wav_log      = default_enable_wav_log;
int32_t     config_ref_channel         = default_ref_channel;
std::string config_fifo_file           = default_fifo_file;
bool        config_dynamic_doa         = default_dynamic_doa;


static bool stop = false;
static char recv_buffer[RECV_BUFF_LEN];
std::string recv_string;

void signal_handler(int signal)
{
    if (signal == SIGPIPE){
        std::cout << "Caught signal SIGPIPE" << std::endl;
        return;
    }
    std::cout << "Caught signal " << signal << ", terminating..." << std::endl;
    stop = true;
}

static const struct option long_options[] = {
    {"mode", required_argument, NULL, 'm'},
    {"mic-type", required_argument, NULL, 1000},
    {"config", required_argument, NULL, 'c'},
    {"test", no_argument, NULL, 't'},
    {"hotword-engine", required_argument, NULL, 1001},
    {"snowboy-res-path", required_argument, NULL, 1002},
    {"snowboy-model-path", required_argument, NULL, 1003},
    {"snowboy-sensitivity", required_argument, NULL, 1004},
    {"snips-model-path", required_argument, NULL, 1005},
    {"snips-sensitivity", required_argument, NULL, 1006},
    {"source", required_argument, NULL, 's'},
    {"agc-level", required_argument, NULL, 'g'},
    {"ref-channel", required_argument, NULL, 'r'},
    {"fifo-file", required_argument, NULL, 1007},
    {"dynamic-doa", no_argument, NULL, 1008},
    {"enable-wav-log", no_argument, NULL, 'w'},
    {"debug", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {"version",no_argument, NULL, 1009},
    {NULL, 0, NULL, 0}
};

static void help(const char *argv0) {
    std::cout << "Usage: respeakerd [options]" << std::endl;
    std::cout << "respeakerd is a server application for the microphone arrays of SEEED." << std::endl << std::endl;

    std::cout << "  -m, --mode=MODE                          the mode of respeakerd, can be: standard, pulse" << std::endl;
    std::cout << "                                           default: " << default_mode << std::endl;
    std::cout << "      --mic-type=MIC_TYPE                  the type of microphone array, can be CIRCULAR_6MIC, CIRCULAR_4MIC" << std::endl;
    std::cout << "                                           default: " << default_mic_type << std::endl;
    std::cout << "  -t, --test                               test the configuration file and exit" << std::endl;
    std::cout << "      --hotword-engine=STRING              the hotword engine, can be: snowboy, snips" << std::endl;
    std::cout << "                                           default: " << default_hotword_engine << std::endl;
    std::cout << "      --snowboy-res-path=PATH              the path to snowboay's resource file" << std::endl;
    std::cout << "                                           default: " << default_snowboy_res_path << std::endl;
    std::cout << "      --snowboy-model-path=PATH            the path to snowboay's model file" << std::endl;
    std::cout << "                                           default: " << default_snowboy_model_path << std::endl;
    std::cout << "      --snowboy-sensitivity=FLOAT_NUMBER   the sensitivity of snowboy" << std::endl;
    std::cout << "                                           default: " << default_snowboy_sensitivity << std::endl;
    std::cout << "      --snips-model-path=PATH              the path to snips's hotword model files" << std::endl;
    std::cout << "                                           default: " << default_snips_model_path << std::endl;
    std::cout << "      --snips-sensitivity=FLOAT_NUMBER     the sensitivity of snips hotword engine" << std::endl;
    std::cout << "                                           default: " << default_snips_sensitivity << std::endl;
    std::cout << "  -s, --source=STRING                      the source of pulseaudio from which the audio stream is pulled" << std::endl;
    std::cout << "                                           default: " << default_source << std::endl;
    std::cout << "  -g, --agc-level=INTEGER                  target dBFS for AGC, the range is [-31, 0]" << std::endl;
    std::cout << "                                           e.g. -g \"-3\" or --agc-level=\"-3\", default: " << default_agc_level << std::endl;
    std::cout << "  -r, --ref-channel=INTEGER                the channel index of the AEC reference, 6 or 7" << std::endl;
    std::cout << "                                           default: " << default_ref_channel << std::endl;
    std::cout << "      --fifo-file=FILE                     the path of the fifo file which is required by the pulse mode" << std::endl;
    std::cout << "                                           default: " << default_fifo_file << std::endl;
    std::cout << "      --dynamic-doa                        if specified, the DoA direction will dynamically track the sound, " << std::endl;
    std::cout << "                                           otherwise it only changes when hotword detected" << std::endl;
    std::cout << "  -w, --enable-wav-log                     enable logging audio streams into wav files for debugging purpose" << std::endl;
    std::cout << "  -v, --debug                              print more messages" << std::endl;
    std::cout << "  -h, --help                               display this help and exit" << std::endl;
    std::cout << "      --version                            output version information and exit" << std::endl;
}

void set_config_from_file(INIReader &reader)
{
    config_mode                = reader.Get("", "mode", default_mode);
    config_mic_type            = reader.Get("", "mic_type", default_mic_type);

    config_hotword_engine      = reader.Get("", "hotword_engine", default_hotword_engine);
    config_snowboy_res_path    = reader.Get("", "snowboy_res_path", default_snowboy_res_path);
    config_snowboy_model_path  = reader.Get("", "snowboy_model_path", default_snowboy_model_path);
    config_snowboy_sensitivity = reader.Get("", "snowboy_sensitivity", default_snowboy_sensitivity);
    config_snips_model_path    = reader.Get("", "snips_model_path", default_snips_model_path);
    config_snips_sensitivity   = reader.GetReal("", "snips_sensitivity", default_snips_sensitivity);

    config_source              = reader.Get("", "source", default_source);
    config_agc_level           = reader.GetInteger("", "agc_level", default_agc_level);
    config_debug               = reader.GetBoolean("", "debug", default_debug);
    config_enable_wav_log      = reader.GetBoolean("", "enable_wav_log", default_enable_wav_log);
    config_ref_channel         = reader.GetInteger("", "ref_channel", default_ref_channel);
    config_fifo_file           = reader.Get("", "fifo_file", default_fifo_file);
    config_dynamic_doa         = reader.GetBoolean("", "dynamic_doa", default_dynamic_doa);

}

int overwrite_config_from_cmdline(int argc, char *const *argv)
{
    int c;
    while ((c = getopt_long(argc, argv, "m:c:s:g:r:twhv", long_options, NULL)) != -1) {
        switch (c) {
        case 'm':
            config_mode = std::string(optarg);
            break;
        case 1000:
            config_mic_type = std::string(optarg);
            break;
        case 't':
            config_test = true;
            break;
        case 1001:
            config_hotword_engine = std::string(optarg);
            break;
        case 1002:
            config_snowboy_res_path = std::string(optarg);
            break;
        case 1003:
            config_snowboy_model_path = std::string(optarg);
            break;
        case 1004:
            config_snowboy_sensitivity = std::string(optarg);
            break;
        case 1005:
            config_snips_model_path = std::string(optarg);
            break;
        case 1006:
            config_snips_sensitivity = std::stod(optarg);
            break;
        case 's':
            config_source = std::string(optarg);
            break;
        case 'g':
            config_agc_level = std::stoi(optarg);
            if (config_agc_level > 0 || config_agc_level < -31) {
                std::cout << "invalid agc-level: " << config_agc_level << ", its range is [-31, 0], e.g. -g \"-3\" or --agc-level=\"-3\"" << std::endl;
                return 1;
            }
            break;
        case 'r':
            config_ref_channel = std::stoi(optarg);
            if (config_ref_channel != 6 && config_ref_channel != 7) {
                std::cout << "invalid ref-channel: " << config_ref_channel << ", it can be: 6, 7" << std::endl;
                return 1;
            }
            break;
        case 1007:
            config_fifo_file = std::string(optarg);
            break;
        case 1008:
            config_dynamic_doa = true;
	        break;
        case 'w':
            config_enable_wav_log = true;
            break;
        case 'v':
            config_debug = true;
            break;
        case 'h' :
            help(argv[0]);
            exit(0);
        case 1009 :
            std::cout << RESPEAKERD_VERSION << std::endl;
            exit(0);
        default:
            exit(1);
        }
    }
    return 0;
}

bool blocking_send(int client, std::string data)
{
    // prepare to send response
    std::string response = data;
    const char* ptr = response.c_str();
    int nleft = response.length();
    int nwritten;
    // loop to be sure it is all sent
    while (nleft) {
        if ((nwritten = send(client, ptr, nleft, 0)) < 0) {
            if (errno == EINTR) {
                // the socket call was interrupted -- try again
                continue;
            } else {
                // an error occurred, so break out
                std::cout << "socket write error" << std::endl;
                return false;
            }
        } else if (nwritten == 0) {
            // the socket is closed
            return false;
        }
        nleft -= nwritten;
        ptr += nwritten;
    }
    return true;
}

std::string cut_line(int client, bool &alive)
{
    int     nfds;
    fd_set  readfds;
    struct  timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;

    size_t idx;
    std::string line;

    while (true) {
        // read until we get a newline
        while ((idx = recv_string.find("\r\n")) == std::string::npos) {
            FD_ZERO(&readfds);
            FD_SET(client, &readfds);
            nfds = select(client+1, &readfds, NULL, NULL, &tv);
            if(nfds < 0){
                std::cout << "select  error" << std::endl;
                return "";
            }else if(nfds == 0){
                //std::cout << "timeout  error" << std::endl;
                return "";
            }else{
                int nread = recv(client, recv_buffer, RECV_BUFF_LEN, 0);
                if (nread < 0) {
                    if (errno == EINTR)
                        // the socket call was interrupted -- try again
                        continue;
                    else
                        // an error occurred, so break out
                        return "";
                } else if (nread == 0) {
                    // the socket is closed
                    alive = false;
                    return "";
                }
                // be sure to use append in case we have binary data
                recv_string.append(recv_buffer, nread);
                //std::cout << recv_string << std::endl;
            }
        }

        line = recv_string.substr(0, idx + 2);
        recv_string.erase(0, idx + 2);

        if (line.find("[0]") != std::string::npos) continue;
        else break;
    }

    return line;
}

bool is_client_alive(int client_sock)
{
    int nfds;
    fd_set  testfds;
    struct  timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;

    FD_ZERO(&testfds);
    FD_SET(client_sock, &testfds);
    nfds = select(client_sock + 1, &testfds, NULL, NULL, &tv);
    if (nfds > 0)
    {
        if (recv(client_sock, recv_buffer, RECV_BUFF_LEN, 0) <= 0) {
            if (errno != EINTR) {
                return false;
            }
        }
    }
    return true;
}

bool file_exist(const char* filename)
{
    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

/**
 * Connect to the DBUS bus and send a broadcast signal
 */
void dbus_send_trigger_signal(DBusConnection *conn, int direction)
{
    DBusMessage *msg;
    DBusMessageIter args;
    dbus_uint32_t serial = 0;

    std::cout << "going to send d-bus signal with direction: " << direction << std::endl;

    // create a signal & check for errors
    msg = dbus_message_new_signal("/io/respeaker/respeakerd", // object name of the signal
                                  "respeakerd.signal", // interface name of the signal
                                  "trigger"); // name of the signal
    if (!msg) {
        std::cout << "create dbus message(signal) failed" << std::endl;
        exit(3);
    }

    // append arguments onto signal
    dbus_message_iter_init_append(msg, &args);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &direction)) {
        std::cout << "create dbus message(signal) failed when adding payload" << std::endl;
        exit(3);
    }

    // send the message and flush the connection
    if (!dbus_connection_send(conn, msg, &serial)) {
        std::cout << "sending dbus message failed" << std::endl;
        exit(3);
    }
    dbus_connection_flush(conn);

    std::cout << "dbus signal sent" << std::endl;

    // free the message
    dbus_message_unref(msg);
}

void dbus_send_ready_signal(DBusConnection *conn)
{
    DBusMessage *msg;
    dbus_uint32_t serial = 0;

    std::cout << "going to send d-bus signal: respeakerd_ready " << std::endl;

    // create a signal & check for errors
    msg = dbus_message_new_signal("/io/respeaker/respeakerd", // object name of the signal
                                  "respeakerd.signal", // interface name of the signal
                                  "respeakerd_ready"); // name of the signal
    if (!msg) {
        std::cout << "create dbus message(signal) failed" << std::endl;
        exit(3);
    }

    // send the message and flush the connection
    if (!dbus_connection_send(conn, msg, &serial)) {
        std::cout << "sending dbus message failed" << std::endl;
        exit(3);
    }
    dbus_connection_flush(conn);

    // free the message
    dbus_message_unref(msg);
}

/**
 * Pop a singal message from the dbus if there's any
 */
DBusMessage* dbus_pop_message(DBusConnection *conn)
{
    DBusMessage *msg;

    // non blocking read of the next available message
    dbus_connection_read_write(conn, 0);
    msg = dbus_connection_pop_message(conn);

    return msg;
}

int main(int argc, char *argv[])
{
    // signal process
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = signal_handler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);
    sigaction(SIGTERM, &sig_int_handler, NULL);
    sigaction(SIGPIPE, &sig_int_handler, NULL);


    INIReader reader(CONFIG_FILE);
    int parse_result = reader.ParseError();
    if (parse_result < 0) {
        std::cout << "can not open " << CONFIG_FILE << ", ";
        std::cout << "will use cmdline options." << std::endl;
    } else {
        if (parse_result > 0) {
            std::cout << "parse error in " << CONFIG_FILE << ", line " << parse_result << "." << std::endl;
            exit(1);
        }
        set_config_from_file(reader);
    }

    // detect which options are set on the command line, and overwrite the value from conf file
    if (overwrite_config_from_cmdline(argc, argv) != 0) {
        exit(1);
    }

    int mode = MODE_STANDARD; //standard
    if (config_mode == "pulse") {
        mode = MODE_PULSE;
    } else if (config_mode == "manual_with_kws") {
        mode = MODE_M_WITH_KWS;
    } else if (config_mode == "manual_without_kws") {
        mode = MODE_M_WITHOUT_KWS;
    }

    int kws_mode = KWS_SNIPS; // snips
    if (mode == MODE_M_WITHOUT_KWS) {
        kws_mode = KWS_NONE;
    } else if (config_hotword_engine == "snowboy") {
        kws_mode = KWS_SNOWBOY;
    }

    MicType _mic_type = CIRCULAR_6MIC_7BEAM;
    if (config_mic_type == "CIRCULAR_4MIC") _mic_type = CIRCULAR_4MIC_9BEAM;
    // else if (config_mic_type == "LINEAR_6MIC") _mic_type = LINEAR_6MIC_8BEAM;
    // else if (config_mic_type == "LINEAR_4MIC") _mic_type = LINEAR_4MIC_1BEAM;

    std::cout << "configuration file: " << CONFIG_FILE << std::endl;
    std::cout << "==========================================================" << std::endl;
    std::cout << "parameters" << std::endl;
    std::cout << "----------------------------------------------------------" << std::endl;
    if (mode == MODE_STANDARD) std::cout << "mode: standard" << std::endl;
    else std::cout << "mode: pulse" << std::endl;
    std::cout << "mic_type: " << config_mic_type << std::endl;

    std::cout << "hotword_engine: " << config_hotword_engine << std::endl;
    std::cout << "snowboy_res_path: " << config_snowboy_res_path << std::endl;
    std::cout << "snowboy_model_path: " << config_snowboy_model_path << std::endl;
    std::cout << "snowboy_sensitivity: " << config_snowboy_sensitivity << std::endl;
    std::cout << "snips_model_path: " << config_snips_model_path << std::endl;
    std::cout << "snips_sensitivity: " << config_snips_sensitivity << std::endl;

    std::cout << "source: " << config_source << std::endl;
    std::cout << "agc_level: " << config_agc_level << std::endl;
    std::cout << "debug: " << config_debug << std::endl;
    std::cout << "enable_wav_log: " << config_enable_wav_log << std::endl;
    std::cout << "ref_channel: " << config_ref_channel << std::endl;
    std::cout << "fifo_file: " << config_fifo_file << std::endl;
    std::cout << "dynamic_doa: " << config_dynamic_doa << std::endl;
    std::cout << "==========================================================" << std::endl;

    if (config_test) exit(0);

    // init librespeaker
    std::unique_ptr<PulseCollectorNode> collector;
    std::unique_ptr<VepAecBeamformingNode> vep_aec_bf;
    std::unique_ptr<Snips1bDoaKwsNode> snips_kws;
    std::unique_ptr<Snowboy1bDoaKwsNode> snowboy_kws;
    std::unique_ptr<ReSpeaker> respeaker;
    collector.reset(PulseCollectorNode::Create_48Kto16K(config_source, BLOCK_SIZE_MS));
    vep_aec_bf.reset(VepAecBeamformingNode::Create(_mic_type, true, config_ref_channel, config_enable_wav_log));

    if (kws_mode == KWS_SNIPS) {
        snips_kws.reset(Snips1bDoaKwsNode::Create(config_snips_model_path,
                                                config_snips_sensitivity,
                                                true,
                                                false));
        snips_kws->DisableAutoStateTransfer();
        snips_kws->SetAgcTargetLevelDbfs((int)std::abs(config_agc_level));
        // snips_kws->SetTriggerPostConfirmThresholdTime(160);
        // snips_kws->SetAutoDoaUpdate(config_dynamic_doa);
    }
    else if (kws_mode == KWS_SNOWBOY) {
        snowboy_kws.reset(Snowboy1bDoaKwsNode::Create(config_snowboy_res_path,
                                                    config_snowboy_model_path,
                                                    config_snowboy_sensitivity));
        snowboy_kws->DisableAutoStateTransfer();
        snowboy_kws->SetAgcTargetLevelDbfs((int)std::abs(config_agc_level));
        // snowboy_kws->SetTriggerPostConfirmThresholdTime(160);
        snowboy_kws->SetAutoDoaUpdate(config_dynamic_doa);
    }
    else {
        // should be a not_kws_node here
    }

    vep_aec_bf->Uplink(collector.get());

    if (config_debug) {
        respeaker.reset(ReSpeaker::Create(DEBUG_LOG_LEVEL));
    } else {
        respeaker.reset(ReSpeaker::Create(INFO_LOG_LEVEL));
    }
    respeaker->RegisterChainByHead(collector.get());

    if (kws_mode == KWS_SNIPS) {
        // collector->SetThreadPriority(60);  // the default priority is 50
        // vep_aec_bf->SetThreadPriority(59);
        // snips_kws->SetThreadPriority(58);
        snips_kws->Uplink(vep_aec_bf.get());

        respeaker->RegisterDirectionManagerNode(snips_kws.get());
        respeaker->RegisterHotwordDetectionNode(snips_kws.get());
        respeaker->RegisterOutputNode(snips_kws.get());
    }
    else if (kws_mode == KWS_SNOWBOY) {
        // collector->SetThreadPriority(60);
        // vep_aec_bf->SetThreadPriority(59);
        // snowboy_kws->SetThreadPriority(58);
        snowboy_kws->Uplink(vep_aec_bf.get());
        respeaker->RegisterDirectionManagerNode(snowboy_kws.get());
        respeaker->RegisterHotwordDetectionNode(snowboy_kws.get());
        respeaker->RegisterOutputNode(snowboy_kws.get());
    }
    else if (kws_mode == KWS_NONE) {
        // collector->SetThreadPriority(60);
        // vep_aec_bf->SetThreadPriority(59);
        respeaker->RegisterOutputNode(vep_aec_bf.get());
    }


    int sock, client_sock, rval, un_size, fd;
    struct sockaddr_un server, new_addr;
    char buf[1024];
    bool socket_error, detected, cloud_ready;
    // support multi-hotword and vad
    int hotword_index;
    bool vad_status = false;
    int dir;
    std::string one_block, one_line;
    std::string event_pkt_str, audio_pkt_str;
    int frames;
    size_t base64_len;

    uint16_t tick;

    TimePoint on_detected, cur_time, on_speak;

    SNDFILE	*snd_file;
    SF_INFO	sfinfo;

    DBusConnection *dbus_conn;
    DBusError dbus_err;

    if (mode == MODE_STANDARD) {  // standard mode
        // init the socket
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cout << "error when opening stream socket" << std::endl;
            exit(1);
        }
        // init bind
        memset(&server, 0, sizeof(server));
        server.sun_family = AF_UNIX;
        strcpy(server.sun_path, SOCKET_FILE);
        unlink(SOCKET_FILE);
        if (bind(sock, (struct sockaddr *) &server, sizeof(struct sockaddr_un)) < 0){
            std::cout << "error when binding stream socket" << std::endl;
            close(sock);
            exit(1);
        }
        if (chmod(SOCKET_FILE, S_IRWXU | S_IRWXG | S_IRWXO) < 0) {
            std::cout << "error when changing the permission of the socket file" << std::endl;
            close(sock);
            exit(1);
        }
        // only accept 1 client
        if ((listen(sock, 1)) < 0){
            std::cout << "cannot listen the client connect request" << std::endl;
            close(sock);
            exit(1);
        }

        std::cout << "now waiting for client connections ..." << std::endl;

    } else {  // pulse mode
        while (!file_exist(config_fifo_file.c_str())) {
            std::cout << "fifo file does not exist: " << config_fifo_file << ", retry in 1 second ..." << std::endl;
            auto delay = std::chrono::seconds(1);
            std::this_thread::sleep_for(delay);
            if (stop) exit(2);
        }

        std::cout << "get d-bus connection ..." << std::endl;

        // initialise the errors
        dbus_error_init(&dbus_err);

        // connect to the bus and check for errors
        dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_err);
        if (dbus_error_is_set(&dbus_err)) {
            std::cout << "d-bus connection error: " << dbus_err.message << std::endl;
            dbus_error_free(&dbus_err);
        }
        if (!dbus_conn) {
            exit(3);
        }

        dbus_bus_add_match(dbus_conn, "type='signal',interface='respeakerd.signal'", &dbus_err); // see signals from the given interface
        dbus_connection_flush(dbus_conn);
        if (dbus_error_is_set(&dbus_err)) {
            std::cout << "d-bus add_match error: " << dbus_err.message << std::endl;
            exit(3);
        }
    }


    while(!stop){
        if (mode == MODE_STANDARD) {
            un_size = sizeof(struct sockaddr_un);
            client_sock = accept(sock, (struct sockaddr *)&new_addr, (socklen_t *)&un_size);
            if (client_sock == -1) {
                std::cout << "socket accept error" << std::endl;
                continue;
            }
            std::cout << "accepted socket client: " << client_sock << std::endl;
        } else {
            fd = open(config_fifo_file.c_str(), O_WRONLY);  // will block here if there's no read-side on this fifo
            std::cout << "connected to fifo file: " << config_fifo_file << std::endl;
        }

        if (!respeaker->Start(&stop)) {
            std::cout << "Can not start the respeaker node chain." << std::endl;
            return -1;
        }

        size_t num_channels = respeaker->GetNumOutputChannels();
        int rate = respeaker->GetNumOutputRate();
        std::cout << "respeakerd output: num channels: " << num_channels << ", rate: " << rate << std::endl;

        respeaker->Pause();
        std::cout << "waiting for ready signal..." << std::endl;

        cur_time = SteadyClock::now();
        socket_error = false;
        cloud_ready = false;

        // wait the cloud to be ready
        while (!stop && !socket_error)
        {
            if (mode == MODE_STANDARD) {
                // wait for the ready signal
                bool alive = true;
                one_line = cut_line(client_sock, alive);
                if (!alive) {
                    std::cout << "client socket is closed, drop it" << std::endl;
                    socket_error = true;
                    break;
                }
                if (one_line != "") {
                    //std::cout << one_line << std::endl;
                    if(one_line.find("ready") != std::string::npos) {
                        cloud_ready = true;
                        std::cout << "cloud ready" << std::endl;
                        break;
                    }
                }

            } else {
#if 0
                // wait on d-bus for the 'ready' state
                DBusMessage *msg = dbus_pop_message(dbus_conn);
                if (msg) {
                    if (dbus_message_is_signal(msg, "respeakerd.signal", "ready")) {
                        cloud_ready = true;
                        std::cout << "cloud ready" << std::endl;
                    }
                    dbus_message_unref(msg);
                    if (cloud_ready) break;
                }
#endif
                cloud_ready = true;  // we dont need to wait cloud ready for Pulse mode
                std::cout << "assuming cloud always ready for PulseAudio mode" << std::endl;

                dbus_send_ready_signal(dbus_conn);

                break;
            }

            if (SteadyClock::now() - cur_time > std::chrono::milliseconds(WAIT_READY_TIMEOUT)) {
                std::cout << "wait ready timeout" << std::endl;
                socket_error = true;
                break;
            }
        }

        if (!socket_error) respeaker->Resume();

        // init libsndfile
        if (config_enable_wav_log) {
            memset(&sfinfo, 0, sizeof(sfinfo));
            sfinfo.samplerate	= rate ;
            sfinfo.channels		= num_channels ;
            sfinfo.format		= (SF_FORMAT_WAV | SF_FORMAT_PCM_24) ;
            std::ostringstream   file_name;
            file_name << "record_respeakerd_" << client_sock << ".wav";
            if (! (snd_file = sf_open(file_name.str().c_str(), SFM_WRITE, &sfinfo)))
            {
                std::cout << "Error : Not able to open output file." << std::endl;
                return -1 ;
            }
        }

        tick = 0;
        while (!stop && !socket_error)
        {
            // check the status or events from the client side
            if (mode == MODE_STANDARD) {
                bool alive = true;
                do {
                    one_line = cut_line(client_sock, alive);
                    if (!alive) {
                        std::cout << "client socket is closed, drop it" << std::endl;
                        socket_error = true;
                        break;
                    }
                    if (one_line != "") {
                        //std::cout << one_line << std::endl;
                        if (one_line.find("ready") != std::string::npos) {
                            cloud_ready = true;
                            //std::cout << "cloud ready" << std::endl;
                        } else if (one_line.find("connecting") != std::string::npos) {
                            cloud_ready = false;
                            std::cout << "cloud is reconnecting..." << std::endl;
                        } else if (one_line.find("on_speak") != std::string::npos) {
                            on_speak = SteadyClock::now();
                            if (config_debug) std::cout << "on_speak..." << std::endl;
                            //respeaker->SetChainState(WAIT_TRIGGER_QUIETLY);
                        }
                    }
                } while (one_line != "");
            } else {
                // check singals on dbus
                while (1) {
                    DBusMessage *msg = dbus_pop_message(dbus_conn);
                    if (msg) {
                        if (dbus_message_is_signal(msg, "respeakerd.signal", "ready")) {
                            cloud_ready = true;
                        } else if (dbus_message_is_signal(msg, "respeakerd.signal", "connecting")) {
                            cloud_ready = false;
                            std::cout << "cloud is reconnecting..." << std::endl;
                        } else if (dbus_message_is_signal(msg, "respeakerd.signal", "on_speak")) {
                            on_speak = SteadyClock::now();
                            if (config_debug) std::cout << "on_speak..." << std::endl;
                            //respeaker->SetChainState(WAIT_TRIGGER_QUIETLY);
                        }
                        dbus_message_unref(msg);
                    } else break;
                }
            }

            if (kws_mode == KWS_SNIPS) {
                if (config_debug && tick++ % 12 == 0) {
                    std::cout << "collector depth: " << collector->GetQueueDeepth() << ", vep_aec_bf: " <<
                    vep_aec_bf->GetQueueDeepth() << ", snips: " << snips_kws->GetQueueDeepth() << std::endl;
                }
            }
            else if (kws_mode == KWS_SNOWBOY) {
                if (config_debug && tick++ % 12 == 0) {
                    std::cout << "collector depth: " << collector->GetQueueDeepth() << ", vep_aec_bf: " <<
                    vep_aec_bf->GetQueueDeepth() << ", snowboy: " << snowboy_kws->GetQueueDeepth() << std::endl;
                }
            }
            // else if (kws_mode == 2) {
            //     if (config_debug && tick++ % 12 == 0) {
            //         std::cout << "collector depth: " << collector->GetQueueDeepth() << ", vep_aec_bf: " <<
            //         vep_aec_bf->GetQueueDeepth() << ", no_kws: " << no_kws->GetQueueDeepth() << std::endl;
            //     }
            // }

            //if have a client connected,this respeaker always detect hotword,if there are hotword,send event and audio.
            one_block = respeaker->DetectHotword(hotword_index);
            if (kws_mode == KWS_SNOWBOY) vad_status = respeaker->GetVad();
            if (config_dynamic_doa) dir = respeaker->GetDirection();

            if (config_enable_wav_log) {
                frames = one_block.length() / (sizeof(int16_t) * num_channels);
                sf_writef_short(snd_file, (const int16_t *)(one_block.data()), frames);
            }

            if (config_debug && (hotword_index >= 1) && (SteadyClock::now() - on_speak) <= std::chrono::milliseconds(SKIP_KWS_TIME_ON_SPEAK)) {
                std::cout << "detected, but skipped!" << std::endl;
            }

            if (mode == MODE_STANDARD) {

                if ((hotword_index >= 1) && cloud_ready && (SteadyClock::now() - on_speak) > std::chrono::milliseconds(SKIP_KWS_TIME_ON_SPEAK)) {
                    if (!config_dynamic_doa) dir = respeaker->GetDirection();
                    on_detected = SteadyClock::now();

                    // send the event to client right now
                    json event = {{"type", "event"}, {"data", "hotword"}, {"direction", dir}, {"index", hotword_index}};
                    event_pkt_str = event.dump();
                    event_pkt_str += "\r\n";

                    if (!blocking_send(client_sock, event_pkt_str)) {
                        socket_error = true;
                    }
                }
                if (cloud_ready) {
                    json audio = {{"type", "audio"}, {"data", base64::encode(one_block)}, {"direction", dir}, {"vad", vad_status}};
                    audio_pkt_str = audio.dump();
                    audio_pkt_str += "\r\n";

                    if (!blocking_send(client_sock, audio_pkt_str)) {
                        socket_error = true;
                    }
                }
            } else {
                if ((hotword_index >= 1) && (SteadyClock::now() - on_speak) > std::chrono::milliseconds(SKIP_KWS_TIME_ON_SPEAK)) {
                    if (!config_dynamic_doa) dir = respeaker->GetDirection();
                    dbus_send_trigger_signal(dbus_conn, dir);
                }
                // pulse mode, we just write the audio data into the fifo file
               int ret = write(fd, one_block.data(), one_block.length());

               if (ret < 0) {
                   socket_error = true;
               }
            }
        } // while
        respeaker->Stop();
        std::cout << "librespeaker cleanup done." << std::endl;

        if (config_enable_wav_log) {
            sf_close(snd_file);
        }

        if (mode == MODE_STANDARD) {
            close(client_sock);
        } else {
            close(fd);
        }
    }

    if (mode == MODE_STANDARD) close(sock);

    return 0;
}

