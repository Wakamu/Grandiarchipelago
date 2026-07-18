#pragma once

#include <string>

namespace grandia_ap {

void StartPipeBridge();
void StopPipeBridge();
void PipeEnqueueLocationCheck(unsigned location_id);
// Area lockout: client filters to progression items only.
// message is a full pipe line without trailing newline, e.g. "LOCKOUT 0020 47522A01 ..."
void PipeEnqueueLockoutMessage(const std::string& message);
// Tell the bridge the save's last applied ReceivedItems count (AP Index).
// Bridge skips redelivery for items with Index <= this value.
// force=true: always announce (confirm-load). force=false: skip if same index already sent.
void PipeEnqueueSync(unsigned received_index, bool force = false);
void PipePoll();

}  // namespace grandia_ap
