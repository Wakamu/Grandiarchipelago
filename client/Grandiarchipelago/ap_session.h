#pragma once

namespace grandia_ap {

class ApSession {
public:
    void Poll();
    void EnqueueLocationCheck(unsigned location_id);
};

ApSession& GetApSession();

}  // namespace grandia_ap
