/*
 * hktuner_ui — native Haiku (Interface Kit) UI for the hktuner chromatic
 * tuner.
 *
 * UI type <https://github.com/rations/LV2-haiku/ns/ui-haiku#HaikuUI>:
 * instantiate() returns a detached BView* as the LV2UI_Widget; the host
 * attaches it to its own BWindow and calls port_event() and cleanup() with
 * that window's looper locked. The UI never touches plugin or RT state —
 * the reference frequency goes to the host through write_function, and the
 * detected frequency/clarity arrive via port_event.
 *
 * The screen is composed from pre-rendered PNG sprites (gui/ in the bundle)
 * layered over the bezel background into an offscreen BBitmap, then blitted.
 * The bezel art sits on a transparent canvas, so the view crops to the
 * device (all sprite geometry is shifted by the crop origin) and paints a
 * black outline around it; the reference-frequency field lives on the black
 * strip below the device. Sprites are untrusted bundle data: every load is
 * NULL-checked and a missing sprite degrades to a plain-text readout, never
 * a crash.
 */
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include <Bitmap.h>
#include <Message.h>
#include <StorageDefs.h>
#include <TextControl.h>
#include <TranslationUtils.h>
#include <View.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HKTUNER_URI "https://github.com/rations/hktuner"
#define HKTUNER_UI_URI HKTUNER_URI "#ui"

enum { HKT_PORT_REF_FREQ = 0, HKT_PORT_FREQ = 1, HKT_PORT_CLARITY = 2 };

static const int32 kMsgRefChanged = 'hkRf';

static const float kRefDefault = 440.0f;
static const float kRefMin = 380.0f;
static const float kRefMax = 500.0f;

/* In-tune window (cents) — the gate's "green" criterion. */
static const double kInTuneCents = 3.0;

/* Sprite geometry, mirrored from gui/geometry.sh: positions are in the
   full 768x512 background frame and drawn shifted by the crop origin. */
static const int kCropX = 74;
static const int kCropY = 132;
static const int kViewW = 626;
static const int kViewH = 268;
static const int kLedRows = 7;
static const int kLedLeftX = 106;
static const int kLedRightX = 614;
static const int kLedStackTop = 165;
static const int kLedPitchY = 22;
static const int kNDots = 51;
static const int kDotSpr = 12;
static const int kDotPitch = 8;
static const int kDotStripY = 305;
static const int kDotStripX0 = 184;
static const int kMarkerW = 16;
static const int kMarkerH = 32;
static const int kGlyphW = 84;
static const int kGlyphX = 322;
static const int kGlyphY = 178;
static const int kSignX = 416;
static const int kSignY = 180;
static const int kDigitX = 416;
static const int kDigitY = 238;

/* Reference-entry strip, in view (already-cropped) coordinates. */
static const int kRefFieldL = 298;
static const int kRefFieldT = 224;
static const int kRefFieldR = 388;
static const int kRefFieldB = 252;

/* Chromatic spelling, sharp-only (LED tuner convention). */
static const struct {
    char letter;
    bool sharp;
} kNotes[12] = {{'C', false}, {'C', true},  {'D', false}, {'D', true},  {'E', false}, {'F', false},
                {'F', true},  {'G', false}, {'G', true},  {'A', false}, {'A', true},  {'B', false}};

/* Background-frame coordinates -> view coordinates. */
static BPoint at(int x, int y)
{
    return BPoint((float)(x - kCropX), (float)(y - kCropY));
}

/* Load one sprite from <bundle>/gui/. NULL (never a crash) on any failure. */
static BBitmap *load_sprite(const char *bundle_path, const char *name)
{
    if (!bundle_path || !name) {
        return NULL;
    }
    const size_t len = strlen(bundle_path);
    const char *sep = (len > 0 && bundle_path[len - 1] == '/') ? "" : "/";
    char path[B_PATH_NAME_LENGTH];
    if (snprintf(path, sizeof(path), "%s%sgui/%s", bundle_path, sep, name) >= (int)sizeof(path)) {
        return NULL;
    }
    return BTranslationUtils::GetBitmapFile(path);
}

/* The whole widget: cropped LCD art + sprite layers (double-buffered) with
   the reference-frequency entry on the black strip below the device. */
class HktunerView : public BView
{
public:
    HktunerView(const char *bundle_path, LV2UI_Write_Function write, LV2UI_Controller controller)
        : BView("hktuner_ui", B_WILL_DRAW), fWrite(write), fController(controller), fOff(NULL),
          fOffView(NULL), fFreq(0.0f), fClarity(0.0f), fRef(kRefDefault)
    {
        fBackground = load_sprite(bundle_path, "background.png");
        fLedOff = load_sprite(bundle_path, "led_off.png");
        fLedGreen = load_sprite(bundle_path, "led_green.png");
        fLedAmber = load_sprite(bundle_path, "led_amber.png");
        fLedRed = load_sprite(bundle_path, "led_red.png");
        fDotOff = load_sprite(bundle_path, "dot_off.png");
        fDotOn = load_sprite(bundle_path, "dot_on.png");
        fDotMarker = load_sprite(bundle_path, "dot_marker.png");
        fDotMarkerGreen = load_sprite(bundle_path, "dot_marker_green.png");
        fDash = load_sprite(bundle_path, "glyph_dash.png");
        fSharp = load_sprite(bundle_path, "glyph_sharp.png");
        for (int i = 0; i < 7; i++) {
            char name[32];
            snprintf(name, sizeof(name), "glyph_%c.png", 'A' + i);
            fGlyphs[i] = load_sprite(bundle_path, name);
        }
        for (int i = 0; i <= 8; i++) {
            char name[32];
            snprintf(name, sizeof(name), "digit_%d.png", i);
            fDigits[i] = load_sprite(bundle_path, name);
        }

        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(kViewW - 1, kViewH - 1));
        SetExplicitMaxSize(BSize(kViewW - 1, kViewH - 1));
        SetExplicitPreferredSize(BSize(kViewW - 1, kViewH - 1));

        fRefInput = new BTextControl(BRect(kRefFieldL, kRefFieldT, kRefFieldR, kRefFieldB),
                                     "ref_freq", NULL, "440.0", new BMessage(kMsgRefChanged));
        /* The field floats on the black outline; the label is drawn by
           Compose() so no control background patch shows. */
        fRefInput->SetViewColor(0, 0, 0);
        fRefInput->SetLowColor(0, 0, 0);
        AddChild(fRefInput);
    }

    virtual ~HktunerView()
    {
        if (fOff) {
            if (fOffView) {
                fOff->RemoveChild(fOffView);
                delete fOffView;
            }
            delete fOff;
        }
        delete fBackground;
        delete fLedOff;
        delete fLedGreen;
        delete fLedAmber;
        delete fLedRed;
        delete fDotOff;
        delete fDotOn;
        delete fDotMarker;
        delete fDotMarkerGreen;
        delete fDash;
        delete fSharp;
        for (int i = 0; i < 7; i++) {
            delete fGlyphs[i];
        }
        for (int i = 0; i <= 8; i++) {
            delete fDigits[i];
        }
    }

    virtual void AttachedToWindow()
    {
        BView::AttachedToWindow();
        const BRect frame(0, 0, kViewW - 1, kViewH - 1);
        fOff = new (std::nothrow) BBitmap(frame, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
        if (fOff && fOff->IsValid()) {
            fOffView = new (std::nothrow) BView(frame, "hktuner_off", B_FOLLOW_NONE, 0);
            if (fOffView) {
                fOff->AddChild(fOffView);
            }
        }
        if (!fOffView) {
            /* Offscreen path unavailable: fall back to direct drawing. */
            delete fOff;
            fOff = NULL;
        }
        fRefInput->SetTarget(this);
    }

    virtual void Draw(BRect update_rect)
    {
        (void)update_rect;
        if (fOff && fOffView && fOff->Lock()) {
            Compose(fOffView);
            fOffView->Sync();
            fOff->Unlock();
            SetDrawingMode(B_OP_COPY);
            DrawBitmap(fOff, BPoint(0, 0));
        } else {
            Compose(this);
        }
    }

    virtual void MessageReceived(BMessage *msg)
    {
        if (msg->what == kMsgRefChanged) {
            ApplyRefText();
            return;
        }
        BView::MessageReceived(msg);
    }

    /* Called by the host (looper locked) on control-port changes. */
    void SetRefFreq(float ref)
    {
        if (!isfinite(ref)) {
            return;
        }
        if (ref < kRefMin) {
            ref = kRefMin;
        }
        if (ref > kRefMax) {
            ref = kRefMax;
        }
        fRef = ref;
        SetRefText(ref);
        Invalidate();
    }

    void SetFreq(float freq)
    {
        if (!isfinite(freq) || freq < 0.0f) {
            freq = 0.0f;
        }
        fFreq = freq;
        Invalidate();
    }

    void SetClarity(float clarity)
    {
        fClarity = clarity;
    }

private:
    void SetRefText(float ref)
    {
        char text[32];
        snprintf(text, sizeof(text), "%.1f", ref);
        fRefInput->SetText(text);
    }

    /* Enter in the text field: parse, clamp, write to port 0; junk input
       restores the last good value. */
    void ApplyRefText()
    {
        const char *text = fRefInput->Text();
        char *end = NULL;
        const float parsed = text ? strtof(text, &end) : 0.0f;
        while (end && (*end == ' ' || *end == '\t')) {
            end++;
        }
        if (!text || !end || end == text || *end != '\0' || !isfinite(parsed)) {
            SetRefText(fRef);
            return;
        }
        float ref = parsed;
        if (ref < kRefMin) {
            ref = kRefMin;
        }
        if (ref > kRefMax) {
            ref = kRefMax;
        }
        fRef = ref;
        SetRefText(ref);
        Invalidate();
        if (fWrite) {
            fWrite(fController, HKT_PORT_REF_FREQ, sizeof(float), 0, &ref);
        }
    }

    void Compose(BView *v)
    {
        /* Opaque black base first: the bezel art has transparent borders. */
        v->SetDrawingMode(B_OP_COPY);
        v->SetHighColor(0, 0, 0);
        v->FillRect(BRect(0, 0, kViewW - 1, kViewH - 1));

        v->SetDrawingMode(B_OP_ALPHA);
        v->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
        if (fBackground) {
            v->DrawBitmap(fBackground, at(0, 0));
        }

        const bool have = fFreq > 0.0f && fRef > 0.0f;
        int note = 0;
        int octave = 4;
        double cents = 0.0;
        if (have) {
            const double x = 69.0 + 12.0 * log2((double)fFreq / (double)fRef);
            const long s = lround(x);
            cents = 100.0 * (x - (double)s);
            note = (int)(((s % 12) + 12) % 12);
            octave = (int)(s / 12) - 1;
            if (octave < 0) {
                octave = 0;
            }
            if (octave > 8) {
                octave = 8;
            }
        }

        DrawLeds(v, have, cents);
        DrawDots(v, have, cents);
        DrawNote(v, have, note, octave);
        DrawRefLabel(v);
    }

    void DrawLeds(BView *v, bool have, double cents)
    {
        const bool in_tune = have && fabs(cents) <= kInTuneCents;
        int lit = 0;
        if (have && !in_tune) {
            lit = 1 + (int)lround((fabs(cents) - kInTuneCents) / 47.0 * (kLedRows - 1));
            if (lit < 1) {
                lit = 1;
            }
            if (lit > kLedRows) {
                lit = kLedRows;
            }
        }
        for (int row = 0; row < kLedRows; row++) {
            /* row 0 is the bottom cell; stacks fill bottom-up. */
            const int y = kLedStackTop + (kLedRows - 1 - row) * kLedPitchY;
            const BBitmap *hot = (row >= kLedRows - 2) ? fLedRed : fLedAmber;
            const BBitmap *left = fLedOff;
            const BBitmap *right = fLedOff;
            if (in_tune && row == 0) {
                left = fLedGreen;
                right = fLedGreen;
            } else if (row < lit) {
                if (cents < 0.0) {
                    left = hot;
                } else {
                    right = hot;
                }
            }
            if (left) {
                v->DrawBitmap(left, at(kLedLeftX, y));
            }
            if (right) {
                v->DrawBitmap(right, at(kLedRightX, y));
            }
        }
    }

    void DrawDots(BView *v, bool have, double cents)
    {
        for (int i = 0; i < kNDots; i++) {
            /* The center dot stays lit as the zero-cents reference mark. */
            const BBitmap *dot = (i == kNDots / 2) ? fDotOn : fDotOff;
            if (dot) {
                v->DrawBitmap(dot, at(kDotStripX0 + i * kDotPitch, kDotStripY));
            }
        }
        if (!have) {
            return;
        }
        const bool in_tune = fabs(cents) <= kInTuneCents;
        const BBitmap *marker = (in_tune && fDotMarkerGreen) ? fDotMarkerGreen : fDotMarker;
        if (!marker) {
            return;
        }
        int idx = kNDots / 2 + (int)lround(cents / 2.0);
        if (idx < 0) {
            idx = 0;
        }
        if (idx >= kNDots) {
            idx = kNDots - 1;
        }
        v->DrawBitmap(marker, at(kDotStripX0 + idx * kDotPitch + kDotSpr / 2 - kMarkerW / 2,
                                 kDotStripY + kDotSpr / 2 - kMarkerH / 2));
    }

    void DrawNote(BView *v, bool have, int note, int octave)
    {
        if (!have) {
            if (fDash) {
                /* Dash centered over the center dot, not at the note-glyph
                   slot (which sits left of the screen center). */
                const int cx = kDotStripX0 + (kNDots / 2) * kDotPitch + kDotSpr / 2;
                v->DrawBitmap(fDash, at(cx - kGlyphW / 2, kGlyphY));
            }
            return;
        }
        const BBitmap *glyph = fGlyphs[kNotes[note].letter - 'A'];
        const BBitmap *digit = fDigits[octave];
        if (!glyph || !digit) {
            DrawNoteFallback(v, note, octave);
            return;
        }
        v->DrawBitmap(glyph, at(kGlyphX, kGlyphY));
        if (kNotes[note].sharp && fSharp) {
            v->DrawBitmap(fSharp, at(kSignX, kSignY));
        }
        v->DrawBitmap(digit, at(kDigitX, kDigitY));
    }

    /* Missing sprite (corrupt bundle): plain vector text instead. */
    void DrawNoteFallback(BView *v, int note, int octave)
    {
        char text[16];
        snprintf(text, sizeof(text), "%c%s%d", kNotes[note].letter, kNotes[note].sharp ? "#" : "",
                 octave);
        BFont font(be_plain_font);
        font.SetSize(72.0f);
        v->SetFont(&font);
        v->SetDrawingMode(B_OP_OVER);
        v->SetHighColor(216, 240, 255);
        v->DrawString(text, at(kGlyphX, kGlyphY + 84));
        v->SetDrawingMode(B_OP_ALPHA);
        v->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
    }

    void DrawRefLabel(BView *v)
    {
        static const char *label = "A4 (Hz)";
        BFont font(be_plain_font);
        v->SetFont(&font);
        v->SetDrawingMode(B_OP_OVER);
        v->SetHighColor(200, 200, 200);
        const float x = (float)(kRefFieldL - 8) - v->StringWidth(label);
        const float y = (float)(kRefFieldT + kRefFieldB) / 2.0f + 4.0f;
        v->DrawString(label, BPoint(x, y));
    }

    LV2UI_Write_Function fWrite;
    LV2UI_Controller fController;
    BTextControl *fRefInput;
    BBitmap *fBackground;
    BBitmap *fLedOff;
    BBitmap *fLedGreen;
    BBitmap *fLedAmber;
    BBitmap *fLedRed;
    BBitmap *fDotOff;
    BBitmap *fDotOn;
    BBitmap *fDotMarker;
    BBitmap *fDotMarkerGreen;
    BBitmap *fDash;
    BBitmap *fSharp;
    BBitmap *fGlyphs[7];
    BBitmap *fDigits[9];
    BBitmap *fOff;
    BView *fOffView;
    float fFreq;
    float fClarity;
    float fRef;
};

extern "C" {

static LV2UI_Handle ui_instantiate(const LV2UI_Descriptor *descriptor, const char *plugin_uri,
                                   const char *bundle_path, LV2UI_Write_Function write_function,
                                   LV2UI_Controller controller, LV2UI_Widget *widget,
                                   const LV2_Feature *const *features)
{
    (void)descriptor;
    (void)features;
    if (!widget || !plugin_uri || strcmp(plugin_uri, HKTUNER_URI) != 0) {
        return NULL;
    }
    HktunerView *view = new (std::nothrow) HktunerView(bundle_path, write_function, controller);
    if (!view) {
        return NULL;
    }
    *widget = (LV2UI_Widget) static_cast<BView *>(view);
    return (LV2UI_Handle)view;
}

static void ui_cleanup(LV2UI_Handle handle)
{
    HktunerView *view = static_cast<HktunerView *>(handle);
    if (!view) {
        return;
    }
    /* The host calls this with the owning window's looper locked (or the
       view already detached); RemoveSelf is safe in both cases. */
    view->RemoveSelf();
    delete view;
}

static void ui_port_event(LV2UI_Handle handle, uint32_t port_index, uint32_t buffer_size,
                          uint32_t format, const void *buffer)
{
    HktunerView *view = static_cast<HktunerView *>(handle);
    if (!view || format != 0 || buffer_size != sizeof(float) || !buffer) {
        return;
    }
    const float value = *(const float *)buffer;
    switch (port_index) {
        case HKT_PORT_REF_FREQ:
            view->SetRefFreq(value);
            break;
        case HKT_PORT_FREQ:
            view->SetFreq(value);
            break;
        case HKT_PORT_CLARITY:
            view->SetClarity(value);
            break;
        default:
            break;
    }
}

static const void *ui_extension_data(const char *uri)
{
    (void)uri;
    return NULL;
}

static const LV2UI_Descriptor hktuner_ui_descriptor = {
    HKTUNER_UI_URI, ui_instantiate, ui_cleanup, ui_port_event, ui_extension_data,
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
    return (index == 0) ? &hktuner_ui_descriptor : NULL;
}

} /* extern "C" */
