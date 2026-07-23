#pragma once

#include <string>

namespace grandia_ap {

void StartPipeBridge();
void StopPipeBridge();
void PipeEnqueueLocationCheck(unsigned location_id);
// Area lockout: client filters to progression items only.
// message is a full pipe line without trailing newline, e.g. "LOCKOUT 0020 47522A01 ..."
void PipeEnqueueLockoutMessage(const std::string& message);
// Tell the bridge the save's last applied ReceivedItems count (AP Index) + seed hash.
// has_trailer: 1 if a GAP1 trailer was present on disk (vs vanilla 0xE80-only).
// Bridge skips redelivery for items with Index <= this value, and rejects mismatched saves.
// force=true: always announce (confirm-load / first bind). force=false: skip if same already sent.
void PipeEnqueueSync(unsigned received_index, unsigned seed_hash, unsigned has_trailer,
                     bool force = false);
void PipePoll();

}  // namespace grandia_ap
