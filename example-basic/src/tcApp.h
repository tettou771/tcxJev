#pragma once

#include <TrussC.h>
#include <tcxJev.h>

using namespace std;
using namespace tc;
using namespace tcx::jev;

class tcApp : public App {
public:
    void setup() override;
    void draw() override;
    void keyPressed(int key) override;

private:
    // The latest answer for one message
    struct Slot {
        uint64_t requestId = 0;   // 0 = never asked
        bool waiting = false;
        float sentAt = 0;
        float seconds = 0;        // round trip of the last response
        bool hasResponse = false;
        ResponseEventArgs response;
    };

    void ask(int index);
    void askImage(bool locate);
    Image photo;
    uint64_t imageRequestId = 0;
    string imageStatus;
    void select(int index);
    float drawNoul(const string& label, const Answer& a, float x, float y);
    float drawDistribution(const string& label, const Answer& a, float x, float y);

    Client jev;
    EventListener jevListener;

    vector<string> messages;
    vector<Slot> slots;
    int current = 0;
};
