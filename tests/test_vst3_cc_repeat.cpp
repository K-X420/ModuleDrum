// Issue #6: a repeated MIDI CC value must reach the VST3 plugin. Hosts send a CC as a change of a hidden parameter
// (IMidiMapping) and only when its value changes, so the patched JUCE wrapper (ext/patches/0001-juce-vst3-cc-repeat)
// reports that parameter back through outputParameterChanges at a value no CC maps to. This loads the built .vst3
// like a host, sends CC 74 and a normal parameter change through process(), and checks what comes back.
// Windows only (LoadLibrary + InitDll). Usage: test-vst3-cc-repeat <path to the .vst3 binary>
#define INIT_CLASS_IID   // this file defines the interface IDs it uses
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int failures = 0;
void check (bool ok, const std::string& what)
{
    std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

// Stack-owned COM objects: no reference counting
#define NO_REFCOUNT                                   \
    uint32 PLUGIN_API addRef() override  { return 1; } \
    uint32 PLUGIN_API release() override { return 1; }

template <typename I>
tresult answer (I* self, const TUID iid, void** obj)
{
    if (FUnknownPrivate::iidEqual (iid, FUnknown::iid) || FUnknownPrivate::iidEqual (iid, I::iid))
    {
        *obj = self;
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

struct Queue final : IParamValueQueue
{
    explicit Queue (ParamID i) : id (i) {}
    ParamID id;
    std::vector<std::pair<int32, ParamValue>> points;

    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override { return answer<IParamValueQueue> (this, iid, obj); }
    NO_REFCOUNT
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return (int32) points.size(); }
    tresult PLUGIN_API getPoint (int32 index, int32& offset, ParamValue& value) override
    {
        if (index < 0 || index >= (int32) points.size())
            return kResultFalse;
        offset = points[(size_t) index].first;
        value = points[(size_t) index].second;
        return kResultTrue;
    }
    tresult PLUGIN_API addPoint (int32 offset, ParamValue value, int32& index) override
    {
        index = (int32) points.size();
        points.emplace_back (offset, value);
        return kResultTrue;
    }
};

struct Changes final : IParameterChanges
{
    std::deque<Queue> queues;   // deque: pointers handed out stay valid

    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override { return answer<IParameterChanges> (this, iid, obj); }
    NO_REFCOUNT
    int32 PLUGIN_API getParameterCount() override { return (int32) queues.size(); }
    IParamValueQueue* PLUGIN_API getParameterData (int32 index) override
    {
        return index >= 0 && index < (int32) queues.size() ? &queues[(size_t) index] : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData (const ParamID& id, int32& index) override
    {
        for (size_t i = 0; i < queues.size(); ++i)
            if (queues[i].id == id)
            {
                index = (int32) i;
                return &queues[i];
            }
        index = (int32) queues.size();
        return &queues.emplace_back (id);
    }
    Queue* find (ParamID id)
    {
        for (auto& q : queues)
            if (q.id == id)
                return &q;
        return nullptr;
    }
};

struct Host final : IHostApplication
{
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override { return answer<IHostApplication> (this, iid, obj); }
    NO_REFCOUNT
    tresult PLUGIN_API getName (String128 name) override
    {
        const char* n = "test-vst3-cc-repeat";
        for (int i = 0; i < 128; ++i)
            if ((name[i] = (char16) n[i]) == 0)
                break;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance (TUID, TUID, void** obj) override { *obj = nullptr; return kResultFalse; }
};

template <typename I>
I* query (FUnknown* from)
{
    void* p = nullptr;
    return from != nullptr && from->queryInterface (I::iid, &p) == kResultOk ? static_cast<I*> (p) : nullptr;
}

// What a host sends for a CC (cc/127), and the plugin's 7-bit conversion of a value back to a CC
double ccValue (int cc) { return cc / 127.0; }
int toCc (double v) { return std::min (127, std::max (0, (int) (v * 128.0))); }
bool onCcGrid (double v) { return std::abs (v * 127.0 - std::round (v * 127.0)) < 1e-3 || std::abs (v * 128.0 - std::round (v * 128.0)) < 1e-3; }

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: test-vst3-cc-repeat <plugin .vst3 binary>\n");
        return 2;
    }

    HMODULE dll = LoadLibraryA (argv[1]);
    if (dll == nullptr)
    {
        std::printf ("could not load %s\n", argv[1]);
        return 1;
    }
    using InitFn = bool (*)();
    using FactoryFn = IPluginFactory* (PLUGIN_API*)();
    auto initDll = (InitFn) GetProcAddress (dll, "InitDll");
    auto exitDll = (InitFn) GetProcAddress (dll, "ExitDll");
    auto getFactory = (FactoryFn) GetProcAddress (dll, "GetPluginFactory");
    if (getFactory == nullptr || (initDll != nullptr && ! initDll()))
    {
        std::printf ("not a VST3 module\n");
        return 1;
    }

    Host host;
    IPluginFactory* factory = getFactory();
    if (auto* f3 = query<IPluginFactory3> (factory))
    {
        f3->setHostContext (&host);
        f3->release();
    }

    // The audio component, its controller, connected directly as a host does
    IComponent* component = nullptr;
    for (int32 i = 0; i < factory->countClasses() && component == nullptr; ++i)
    {
        PClassInfo info;
        if (factory->getClassInfo (i, &info) == kResultOk && std::strcmp (info.category, kVstAudioEffectClass) == 0)
            factory->createInstance (info.cid, IComponent::iid, (void**) &component);
    }
    if (component == nullptr || component->initialize (&host) != kResultOk)
    {
        std::printf ("no audio component\n");
        return 1;
    }
    TUID controllerCid;
    IEditController* controller = nullptr;
    if (component->getControllerClassId (controllerCid) == kResultOk)
        factory->createInstance (controllerCid, IEditController::iid, (void**) &controller);
    if (controller == nullptr || controller->initialize (&host) != kResultOk)
    {
        std::printf ("no edit controller\n");
        return 1;
    }
    auto* componentCp = query<IConnectionPoint> (component);
    auto* controllerCp = query<IConnectionPoint> (controller);
    if (componentCp != nullptr && controllerCp != nullptr)
    {
        componentCp->connect (controllerCp);
        controllerCp->connect (componentCp);
    }

    auto* mapping = query<IMidiMapping> (controller);
    auto* processor = query<IAudioProcessor> (component);
    check (mapping != nullptr && processor != nullptr, "the plugin offers IMidiMapping and IAudioProcessor");
    if (mapping == nullptr || processor == nullptr)
        return 1;

    // Every parameter ID the plugin maps a MIDI controller to
    std::set<ParamID> ccParams;
    for (int16 ch = 0; ch < 16; ++ch)
        for (CtrlNumber n = 0; n < kCountCtrlNumber; ++n)
        {
            ParamID id;
            if (mapping->getMidiControllerAssignment (0, ch, n, id) == kResultTrue)
                ccParams.insert (id);
        }
    ParamID cc74 = 0, bend = 0;
    check (mapping->getMidiControllerAssignment (0, 0, 74, cc74) == kResultTrue, "CC 74 on channel 1 is mapped to a parameter");
    mapping->getMidiControllerAssignment (0, 0, kPitchBend, bend);

    // A normal (automatable) parameter
    ParamID normal = 0;
    bool haveNormal = false;
    for (int32 i = 0; i < controller->getParameterCount() && ! haveNormal; ++i)
    {
        ParameterInfo info;
        if (controller->getParameterInfo (i, info) == kResultOk && (info.flags & ParameterInfo::kCanAutomate) != 0
            && (info.flags & (ParameterInfo::kIsBypass | ParameterInfo::kIsProgramChange | ParameterInfo::kIsReadOnly)) == 0
            && ccParams.count (info.id) == 0)
        {
            normal = info.id;
            haveNormal = true;
        }
    }
    check (haveNormal, "the plugin has a normal automatable parameter");

    // Audio buffers for every bus, as the plugin declares them
    ProcessSetup setup { kRealtime, kSample32, 512, 48000.0 };
    check (processor->setupProcessing (setup) == kResultOk, "setupProcessing");
    std::vector<std::vector<float>> channelData;
    std::vector<std::vector<float*>> channelPtrs;
    std::vector<AudioBusBuffers> buses[2];
    for (int dir = 0; dir < 2; ++dir)
        for (int32 b = 0; b < component->getBusCount (kAudio, dir == 0 ? kInput : kOutput); ++b)
        {
            BusInfo info;
            component->getBusInfo (kAudio, dir == 0 ? kInput : kOutput, b, info);
            component->activateBus (kAudio, dir == 0 ? kInput : kOutput, b, true);
            channelPtrs.emplace_back();
            for (int32 c = 0; c < info.channelCount; ++c)
                channelPtrs.back().push_back (channelData.emplace_back (512, 0.0f).data());
            AudioBusBuffers abb {};
            abb.numChannels = info.channelCount;
            buses[dir].push_back (abb);
        }
    {   // the pointer lists are complete only now
        size_t k = 0;
        for (int dir = 0; dir < 2; ++dir)
            for (auto& abb : buses[dir])
                abb.channelBuffers32 = channelPtrs[k++].data();
    }
    check (component->setActive (true) == kResultOk, "setActive");
    processor->setProcessing (true);

    // One block: the given input changes in, what the plugin reports out
    auto run = [&] (Changes& in, int32 numSamples) {
        Changes out;
        ProcessData data {};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = numSamples;
        if (numSamples > 0)
        {
            data.numInputs = (int32) buses[0].size();
            data.inputs = buses[0].data();
            data.numOutputs = (int32) buses[1].size();
            data.outputs = buses[1].data();
        }
        data.inputParameterChanges = &in;
        data.outputParameterChanges = &out;
        processor->process (data);
        return out;
    };
    auto send = [&] (ParamID id, int32 offset, double value, int32 numSamples = 256) {
        Changes in;
        int32 index;
        in.addParameterData (id, index)->addPoint (offset, value, index);
        return run (in, numSamples);
    };
    auto expectReset = [&] (Changes& out, int cc, int32 offset, const std::string& what) {
        Queue* q = out.find (cc74);
        const bool one = q != nullptr && q->points.size() == 1;
        check (one, what + ": the CC parameter is reported back");
        if (! one)
            return;
        const auto [o, v] = q->points[0];
        check (o == offset, what + ": at the CC's sample offset (" + std::to_string (o) + ")");
        check (v >= 0.0 && v <= 1.0 && ! onCcGrid (v), what + ": at a value no CC maps to (" + std::to_string (v) + ")");
        check (toCc (v) == cc, what + ": a value that would still convert to CC " + std::to_string (cc));
        controller->setParamNormalized (cc74, v);   // as a host forwards output changes to the controller
    };

    for (int cc : { 80, 80, 30, 0, 63, 64, 126, 127, 127 })
    {
        auto out = send (cc74, 17, ccValue (cc));
        expectReset (out, cc, 17, "CC 74 = " + std::to_string (cc));
    }
    {   // several points in one block: one report, at the last one
        Changes in;
        int32 index;
        auto* q = in.addParameterData (cc74, index);
        q->addPoint (3, ccValue (10), index);
        q->addPoint (40, ccValue (90), index);
        auto out = run (in, 256);
        expectReset (out, 90, 40, "CC 74 = 10 then 90 in one block");
    }
    {   // the parameter flush (no audio) path
        auto out = send (cc74, 0, ccValue (80), 0);
        expectReset (out, 80, 0, "CC 74 = 80 in a parameter flush");
    }
    if (bend != 0)
    {
        auto out = send (bend, 0, 0.75);
        check (out.find (bend) == nullptr, "pitch bend is not reported back");
    }
    if (haveNormal)
    {
        auto out = send (normal, 5, 0.3);
        bool anyCc = false;
        for (auto& q : out.queues)
            anyCc |= ccParams.count (q.id) != 0;
        check (! anyCc, "a normal parameter change reports no MIDI CC parameter");
        Queue* q = out.find (normal);
        std::printf ("      (the normal parameter itself: %s)\n", q == nullptr ? "not reported" : "reported by JUCE");
    }

    processor->setProcessing (false);
    component->setActive (false);
    if (componentCp != nullptr && controllerCp != nullptr)
    {
        componentCp->disconnect (controllerCp);
        controllerCp->disconnect (componentCp);
        componentCp->release();
        controllerCp->release();
    }
    mapping->release();
    processor->release();
    controller->terminate();
    controller->release();
    component->terminate();
    component->release();
    factory->release();
    if (exitDll != nullptr)
        exitDll();
    FreeLibrary (dll);

    std::printf (failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
