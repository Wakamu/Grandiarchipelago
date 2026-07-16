#include "ap_session.h"

#include "pipe_bridge.h"

namespace grandia_ap {

namespace {
ApSession g_session;
}  // namespace

ApSession& GetApSession() { return g_session; }

void ApSession::Poll() {
    PipePoll();
}

void ApSession::EnqueueLocationCheck(unsigned location_id) {
    PipeEnqueueLocationCheck(location_id);
}

}  // namespace grandia_ap
