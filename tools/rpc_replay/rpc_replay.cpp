// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved
//
// If you have any problem, contact us:
//   Baidu Hi : group 1296497 
//   Email    : pbrpc@baidu.com
//   Wiki     : http://wiki.baidu.com/display/RPC/baidu-rpc

#include <gflags/gflags.h>
#include <base/logging.h>
#include <base/time.h>
#include <base/macros.h>
#include <base/file_util.h>
#include <bvar/bvar.h>
#include <bthread/bthread.h>
#include <brpc/channel.h>
#include <brpc/server.h>
#include <brpc/rpc_dump.h>
#include <brpc/serialized_request.h>
#include "info_thread.h"

DEFINE_string(dir, "", "The directory of dumped requests");
DEFINE_int32(times, 1, "Repeat replaying for so many times");
DEFINE_int32(qps, 0, "Limit QPS if this flag is positive");
DEFINE_int32(thread_num, 0, "Number of threads for replaying");
DEFINE_bool(use_bthread, true, "Use bthread to replay");
DEFINE_string(connection_type, "", "Connection type, choose automatically "
              "according to protocol by default");
DEFINE_string(server, "0.0.0.0:8002", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Maximum retry times");
DEFINE_int32(dummy_port, 8899, "Port of dummy server(to monitor replaying)");

bvar::LatencyRecorder g_latency_recorder("rpc_replay");
bvar::Adder<int64_t> g_error_count("rpc_replay_error_count");
bvar::Adder<int64_t> g_sent_count;

// Include channels for all protocols that support both client and server.
class ChannelGroup {
public:
    int Init();

    ~ChannelGroup();

    // Get channel by protocol type.
    brpc::Channel* channel(brpc::ProtocolType type) {
        if ((size_t)type < _chans.size()) {
            return _chans[(size_t)type];
        }
        return NULL;
    }
    
private:
    std::vector<brpc::Channel*> _chans;
};

int ChannelGroup::Init() {
    {
        // force global initialization of rpc.
        brpc::Channel dummy_channel;
    }
    std::vector<std::pair<brpc::ProtocolType, brpc::Protocol> > protocols;
    brpc::ListProtocols(&protocols);
    size_t max_protocol_size = 0;
    for (size_t i = 0; i < protocols.size(); ++i) {
        max_protocol_size = std::max(max_protocol_size,
                                     (size_t)protocols[i].first);
    }
    _chans.resize(max_protocol_size);
    for (size_t i = 0; i < protocols.size(); ++i) {
        if (protocols[i].second.support_client() &&
            protocols[i].second.support_server()) {
            const brpc::ProtocolType prot = protocols[i].first;
            brpc::Channel* chan = new brpc::Channel;
            brpc::ChannelOptions options;
            options.protocol = prot;
            options.connection_type = FLAGS_connection_type;
            options.timeout_ms = FLAGS_timeout_ms/*milliseconds*/;
            options.max_retry = FLAGS_max_retry;
            if (chan->Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(),
                        &options) != 0) {
                LOG(ERROR) << "Fail to initialize channel";
                return -1;
            }
            _chans[prot] = chan;
        }
    }
    return 0;
}

ChannelGroup::~ChannelGroup() {
    for (size_t i = 0; i < _chans.size(); ++i) {
        delete _chans[i];
    }
    _chans.clear();
}

static void handle_response(brpc::Controller* cntl, int64_t start_time,
                            bool sleep_on_error/*note*/) {
    // TODO(gejun): some bthreads are starved when new bthreads are created 
    // continuously, which happens when server is down and RPC keeps failing.
    // Sleep a while on error to avoid that now.
    const int64_t end_time = base::gettimeofday_us();
    const int64_t elp = end_time - start_time;
    if (!cntl->Failed()) {
        g_latency_recorder << elp;
    } else {
        g_error_count << 1;
        if (sleep_on_error) {
            bthread_usleep(10000);
        }
    }
    delete cntl;
}

base::atomic<int> g_thread_offset(0);

static void* replay_thread(void* arg) {
    ChannelGroup* chan_group = static_cast<ChannelGroup*>(arg);
    const int thread_offset = g_thread_offset.fetch_add(1, base::memory_order_relaxed);
    double req_rate = FLAGS_qps / (double)FLAGS_thread_num;
    brpc::SerializedRequest req;
    std::deque<int64_t> timeq;
    size_t MAX_QUEUE_SIZE = (size_t)req_rate;
    if (MAX_QUEUE_SIZE < 100) {
        MAX_QUEUE_SIZE = 100;
    } else if (MAX_QUEUE_SIZE > 2000) {
        MAX_QUEUE_SIZE = 2000;
    }
    timeq.push_back(base::gettimeofday_us());
    for (int i = 0; !brpc::IsAskedToQuit() && i < FLAGS_times; ++i) {
        brpc::SampleIterator it(FLAGS_dir);
        int j = 0;
        for (brpc::SampledRequest* sample = it.Next();
             !brpc::IsAskedToQuit() && sample != NULL; sample = it.Next(), ++j) {
            std::unique_ptr<brpc::SampledRequest> sample_guard(sample);
            if ((j % FLAGS_thread_num) != thread_offset) {
                continue;
            }
            brpc::Channel* chan =
                chan_group->channel(sample->protocol_type());
            if (chan == NULL) {
                LOG(ERROR) << "No channel on protocol="
                           << sample->protocol_type();
                continue;
            }
            
            brpc::Controller* cntl = new brpc::Controller;
            req.Clear();
            
            cntl->reset_rpc_dump_meta(sample_guard.release());
            if (sample->attachment_size() > 0) {
                sample->request.cutn(
                    &req.serialized_data(),
                    sample->request.size() - sample->attachment_size());
                cntl->request_attachment() = sample->request.movable();
            } else {
                req.serialized_data() = sample->request.movable();
            }
            g_sent_count << 1;
            const int64_t start_time = base::gettimeofday_us();
            if (FLAGS_qps <= 0) {
                chan->CallMethod(NULL/*use rpc_dump_context in cntl instead*/,
                        cntl, &req, NULL/*ignore response*/, NULL);
                handle_response(cntl, start_time, true);
            } else {
                google::protobuf::Closure* done =
                    brpc::NewCallback(handle_response, cntl, start_time, false);
                chan->CallMethod(NULL/*use rpc_dump_context in cntl instead*/,
                        cntl, &req, NULL/*ignore response*/, done);
                const int64_t end_time = base::gettimeofday_us();
                int64_t expected_elp = 0;
                int64_t actual_elp = 0;
                timeq.push_back(end_time);
                if (timeq.size() > MAX_QUEUE_SIZE) {
                    actual_elp = end_time - timeq.front();
                    timeq.pop_front();
                    expected_elp = (size_t)(1000000 * timeq.size() / req_rate);
                } else {
                    actual_elp = end_time - timeq.front();
                    expected_elp = (size_t)(1000000 * (timeq.size() - 1) / req_rate);
                }
                if (actual_elp < expected_elp) {
                    bthread_usleep(expected_elp - actual_elp);
                }
            }
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    // Parse gflags. We recommend you to use gflags as well.
    google::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_dir.empty() ||
        !base::DirectoryExists(base::FilePath(FLAGS_dir))) {
        LOG(ERROR) << "--dir=<dir-of-dumped-files> is required";
        return -1;
    }

    if (FLAGS_dummy_port > 0) {
        brpc::StartDummyServerAt(FLAGS_dummy_port);
    }
    
    ChannelGroup chan_group;
    if (chan_group.Init() != 0) {
        LOG(ERROR) << "Fail to init ChannelGroup";
        return -1;
    }

    if (FLAGS_thread_num <= 0) {
        if (FLAGS_qps <= 0) { // unlimited qps
            FLAGS_thread_num = 50;
        } else {
            FLAGS_thread_num = FLAGS_qps / 10000;
            if (FLAGS_thread_num < 1) {
                FLAGS_thread_num = 1;
            }
            if (FLAGS_thread_num > 50) {
                FLAGS_thread_num = 50;
            }
        }
    }

    std::vector<bthread_t> tids;
    tids.resize(FLAGS_thread_num);
    if (!FLAGS_use_bthread) {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&tids[i], NULL, replay_thread, &chan_group) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(
                    &tids[i], NULL, replay_thread, &chan_group) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }
    brpc::InfoThread info_thr;
    brpc::InfoThreadOptions info_thr_opt;
    info_thr_opt.latency_recorder = &g_latency_recorder;
    info_thr_opt.error_count = &g_error_count;
    info_thr_opt.sent_count = &g_sent_count;
    
    if (!info_thr.start(info_thr_opt)) {
        LOG(ERROR) << "Fail to create info_thread";
        return -1;
    }

    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(tids[i], NULL);
        } else {
            bthread_join(tids[i], NULL);
        }
    }
    info_thr.stop();

    return 0;
}
