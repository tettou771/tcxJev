#include "tcApp.h"

// =============================================================================
// tcxJev example-basic
// =============================================================================
// Sends a support message as the state and asks four typed questions about it
// in ONE request: two Nouls (yes/no), a Choice and a Score. The answers come
// back as numbers and are drawn as bars.
//
// API key: bin/data/secrets.json  ->  {"typesafe_api_key": "..."}
// (or the TYPESAFE_API_KEY environment variable, which an app launched from
// Finder or an IDE usually doesn't see). secrets.* is gitignored.
// =============================================================================

namespace {

// The Choice options, in the order they are drawn
const vector<pair<string, string>> kDepartments = {
    {"billing",   "Payments, invoices, refunds, subscriptions"},
    {"technical", "Bugs, outages, integrations, errors"},
    {"sales",     "Pricing, plans, upgrades, new accounts"},
    {"other",     "Anything else, such as thanks or feedback"},
};

// The Score levels, lowest first
const vector<string> kFrustration = {
    "Calm, just stating facts",
    "Mildly annoyed",
    "Frustrated but civil",
    "Very angry, strong language",
};

const Color kBarColor(0.35f, 0.75f, 1.0f);
const Color kPickColor(1.0f, 0.75f, 0.3f);
const float kBarW = 260;
const float kBarH = 10;

// Split text into lines no wider than `width` pixels (bitmap font)
vector<string> wrap(const string& text, float width) {
    vector<string> lines;
    string line, word;
    auto flushWord = [&] {
        if (word.empty()) return;
        string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && getBitmapStringWidth(candidate) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flushWord();
        else word += c;
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void drawBar(float x, float y, float value, const Color& color) {
    setColor(0.22f);
    drawRect(x, y, kBarW, kBarH);
    setColor(color);
    drawRect(x, y, kBarW * clamp(value, 0.0f, 1.0f), kBarH);
}

} // namespace

// -----------------------------------------------------------------------------

void tcApp::setup() {
    messages = {
        "Hi, I've been trying to connect my Stripe account for 3 days and the "
        "integration keeps failing. I'm losing sales. Please help ASAP.",
        "I was charged twice for my March invoice. Please refund the duplicate charge.",
        "What does the Pro plan cost for a team of 20 people? Is there an annual discount?",
        "Your app deleted my project AGAIN. Third time this month. This is absolutely "
        "unacceptable and I want my money back today.",
        "The export button does nothing when I click it. Not urgent, just letting you know.",
        "Thanks for the quick fix yesterday, everything works perfectly now!",
    };
    slots.resize(messages.size());

    // The key file wins over the environment variable Client() already read
    if (fileExists(getDataPath("secrets.json"))) {
        Json secrets = loadJson("secrets.json");
        if (secrets.contains("typesafe_api_key") && secrets["typesafe_api_key"].is_string()) {
            jev.setApiKey(secrets["typesafe_api_key"].get<string>());
        }
    }

    // responseEvent fires on tcxJev's worker thread. Deliver::Main runs this
    // listener on the main thread instead (start of the next frame), so it can
    // safely change what draw() shows.
    jevListener = jev.responseEvent.listen([this](ResponseEventArgs& e) {
        for (auto& slot : slots) {
            if (slot.requestId != e.requestId) continue;
            slot.waiting = false;
            slot.seconds = getElapsedTimef() - slot.sentAt;
            slot.hasResponse = true;
            slot.response = e;
        }
        if (e.ok) {
            logNotice("tcxJev example") << "#" << e.requestId << " " << e.model
                                        << "  urgent " << toString(e.answers["urgent"].noul, 2)
                                        << "  refund " << toString(e.answers["refund"].noul, 2)
                                        << "  department " << e.answers["department"].choice
                                        << "  frustration " << toString(e.answers["frustration"].score, 2)
                                        << "  (" << e.inputTokens << " tokens in)";
        }
    }, Deliver::Main);

    if (jev.hasApiKey()) ask(current);
}

void tcApp::ask(int index) {
    Json departments = Json::object();
    for (auto& [name, description] : kDepartments) departments[name] = description;

    // One request, four questions: all are evaluated against the same state
    // in parallel, so asking more costs almost nothing extra
    uint64_t id = jev.request(Request()
        .state(messages[index])
        .noul("urgent", "Does the message convey urgency or time pressure?")
        .noul("refund", "Does the customer ask for money back?")
        .choice("department", "Which team should handle this message?", departments)
        .score("frustration", "How frustrated does the customer sound?", kFrustration));
    if (id == 0) return;   // not sent; tcxJev logged why

    Slot& slot = slots[index];
    slot.requestId = id;
    slot.waiting = true;
    slot.sentAt = getElapsedTimef();
}

void tcApp::select(int index) {
    current = index;
    if (slots[current].requestId == 0 && jev.hasApiKey()) ask(current);
}

void tcApp::keyPressed(int key) {
    int n = (int)messages.size();
    if (key == KEY_RIGHT || key == KEY_DOWN) select((current + 1) % n);
    else if (key == KEY_LEFT || key == KEY_UP) select((current + n - 1) % n);
    else if (key >= '1' && key < '1' + n) select(key - '1');
    else if (key == KEY_SPACE && jev.hasApiKey()) ask(current);
}

// -----------------------------------------------------------------------------

float tcApp::drawNoul(const string& label, const Answer& a, float x, float y) {
    setColor(0.85f);
    drawBitmapString(label + "  (noul)", x, y);
    drawBar(x, y + 20, (float)a.noul, kBarColor);
    setColor(1.0f);
    drawBitmapString(toString(a.noul, 2), x + kBarW + 12, y + 17);
    return y + 50;
}

float tcApp::drawDistribution(const string& label, const Answer& a, float x, float y) {
    bool isScore = a.type == QuestionType::Score;
    setColor(0.85f);
    drawBitmapString(label + (isScore ? "  (score)" : "  (choice)"), x, y);
    setColor(1.0f);
    string summary = isScore ? "score " + toString(a.score, 2) + " / " + toString(kFrustration.size() - 1)
                             : "-> " + a.choice;
    drawBitmapString(summary + "   confidence " + toString(a.confidence, 2), x, y + 20);
    y += 46;

    // One bar per option / level, in the order they were defined
    size_t rows = isScore ? kFrustration.size() : kDepartments.size();
    for (size_t i = 0; i < rows; i++) {
        string key = isScore ? toString(i) : kDepartments[i].first;
        string name = isScore ? toString(i) + " " + kFrustration[i] : kDepartments[i].first;
        auto it = a.probabilities.find(key);
        float p = it != a.probabilities.end() ? (float)it->second : 0.0f;
        bool picked = isScore ? (int)round(a.score) == (int)i : a.choice == key;

        setColor(picked ? 1.0f : 0.6f);
        drawBitmapString(name, x, y);
        drawBar(x, y + 18, p, picked ? kPickColor : kBarColor);
        setColor(0.8f);
        drawBitmapString(toString(p, 2), x + kBarW + 12, y + 15);
        y += 40;
    }

    // Score: where the probability-weighted position lands between the levels
    if (isScore) {
        float w = kBarW, t = (float)(a.score / (kFrustration.size() - 1));
        setColor(0.4f);
        drawLine(x, y + 6, x + w, y + 6);
        for (size_t i = 0; i < kFrustration.size(); i++) {
            float tx = x + w * i / (kFrustration.size() - 1);
            drawLine(tx, y + 2, tx, y + 10);
        }
        setColor(kPickColor);
        drawCircle(x + w * clamp(t, 0.0f, 1.0f), y + 6, 5);
        y += 24;
    }
    return y;
}

void tcApp::draw() {
    clear(0.08f, 0.09f, 0.11f);
    const float x = 30;

    setColor(1.0f);
    drawBitmapString("tcxJev: one request, four typed answers", x, 24);
    setColor(0.55f);
    drawBitmapString("LEFT / RIGHT or 1-" + toString(messages.size()) + ": message    SPACE: ask again", x, 44);

    // The message, sent as the state
    float y = 84;
    setColor(0.55f);
    drawBitmapString("state (message " + toString(current + 1) + "/" + toString(messages.size()) + ")", x, y);
    y += 22;
    setColor(0.95f, 0.88f, 0.65f);
    for (auto& line : wrap(messages[current], getWindowWidth() - 2 * x)) {
        drawBitmapString(line, x, y);
        y += 18;
    }
    y = max(y + 24, 170.0f);

    if (!jev.hasApiKey()) {
        setColor(1.0f, 0.45f, 0.45f);
        drawBitmapString("No API key.", x, y);
        drawBitmapString("Put {\"typesafe_api_key\": \"...\"} in bin/data/secrets.json,", x, y + 20);
        drawBitmapString("or set the TYPESAFE_API_KEY environment variable.", x, y + 40);
        return;
    }

    const Slot& slot = slots[current];
    if (slot.waiting) {
        setColor(0.55f);
        drawBitmapString("asking Jev...", getWindowWidth() - 150, 84);
    }
    if (!slot.hasResponse) return;

    const ResponseEventArgs& r = slot.response;
    if (!r.ok) {
        setColor(1.0f, 0.45f, 0.45f);
        drawBitmapString("request #" + toString(r.requestId) + " failed", x, y);
        y += 22;
        for (auto& line : wrap(r.error, getWindowWidth() - 2 * x)) {
            drawBitmapString(line, x, y);
            y += 18;
        }
        return;
    }

    // Answers are keyed by the question names used in ask()
    auto answer = [&](const string& name) {
        auto it = r.answers.find(name);
        return it != r.answers.end() ? it->second : Answer();
    };
    float left = y;
    left = drawNoul("urgent", answer("urgent"), x, left);
    left = drawNoul("refund", answer("refund"), x, left);
    left = drawDistribution("department", answer("department"), x, left + 6);
    drawDistribution("frustration", answer("frustration"), getWindowWidth() / 2 + 20, y);

    setColor(0.5f);
    drawBitmapString(r.model + "   " + toString(r.inputTokens) + " tokens in / " + toString(r.outputTokens) +
                     " out   " + toString(slot.seconds, 2) + " s   request #" + toString(r.requestId),
                     x, getWindowHeight() - 30);
}
