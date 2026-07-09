#include <wx/wx.h>
#include <wx/combobox.h>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface CenteredComboBoxCell : NSComboBoxCell
@end

@implementation CenteredComboBoxCell
- (NSRect)centeredTitleRectForBounds:(NSRect)bounds {
	NSRect titleRect = [super titleRectForBounds:bounds];
	CGFloat offset = floor((NSHeight(bounds) - NSHeight(titleRect)) / 2.0);
	if (offset > 0)
		titleRect.origin.y = bounds.origin.y + offset;
	return titleRect;
}

- (NSRect)centeredTextFramePreservingWidth:(NSRect)frame {
	NSRect titleRect = [super titleRectForBounds:frame];
	CGFloat offset = floor((NSHeight(frame) - NSHeight(titleRect)) / 2.0);
	if (offset > 0) {
		frame.origin.y += offset;
		frame.size.height = NSHeight(titleRect);
	}
	return frame;
}

- (NSRect)titleRectForBounds:(NSRect)bounds {
	return [self centeredTitleRectForBounds:bounds];
}

- (void)drawInteriorWithFrame:(NSRect)cellFrame inView:(NSView *)controlView {
	[super drawInteriorWithFrame:[self centeredTextFramePreservingWidth:cellFrame] inView:controlView];
}

- (void)editWithFrame:(NSRect)rect inView:(NSView *)controlView editor:(NSText *)textObj delegate:(id)delegate event:(NSEvent *)event {
	[super editWithFrame:[self centeredTextFramePreservingWidth:rect] inView:controlView editor:textObj delegate:delegate event:event];
}

- (void)selectWithFrame:(NSRect)rect inView:(NSView *)controlView editor:(NSText *)textObj delegate:(id)delegate start:(NSInteger)selStart length:(NSInteger)selLength {
	[super selectWithFrame:[self centeredTextFramePreservingWidth:rect] inView:controlView editor:textObj delegate:delegate start:selStart length:selLength];
}
@end

static NSComboBox *FindNativeComboBox(NSView *view) {
	if ([view isKindOfClass:[NSComboBox class]])
		return (NSComboBox *)view;

	for (NSView *subview in [view subviews]) {
		if (auto combo = FindNativeComboBox(subview))
			return combo;
	}

	return nil;
}

static void CenterNativeComboBoxText(wxComboBox *combo) {
	NSView *view = (NSView *)combo->GetHandle();
	NSComboBox *native_combo = FindNativeComboBox(view);
	if (!native_combo)
		return;

	NSCell *cell = [native_combo cell];
	if (![cell isKindOfClass:[NSComboBoxCell class]])
		return;

	if (![cell isKindOfClass:[CenteredComboBoxCell class]])
		object_setClass(cell, [CenteredComboBoxCell class]);

	[native_combo setNeedsDisplay:YES];
}

static void SizeComboBoxToFit(wxComboBox *combo, wxString const& initial_value, wxArrayString const& choices) {
	wxString widest = initial_value;
	int widest_width = combo->GetTextExtent(widest).GetWidth();

	for (auto const& choice : choices) {
		int width = combo->GetTextExtent(choice).GetWidth();
		if (width > widest_width) {
			widest = choice;
			widest_width = width;
		}
	}

	wxSize best = combo->GetBestSize();
	wxSize size(std::max(combo->GetSizeFromText(widest).GetWidth(), best.GetWidth()), best.GetHeight());
	combo->SetInitialSize(size);
	combo->SetMinSize(size);
	combo->SetMaxSize(wxSize(-1, size.GetHeight()));
}

static wxComboBox *MakeAegisubCombo(wxWindow *parent, wxString const& value, wxString const& widest, wxArrayString const& choices, bool centered) {
	auto combo = new wxComboBox(parent, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
	SizeComboBoxToFit(combo, widest, choices);
	if (centered)
		CenterNativeComboBoxText(combo);
	return combo;
}

class CenteredComboBox final : public wxComboBox {
public:
	CenteredComboBox(wxWindow *parent, wxString const& value, wxArrayString const& choices, long style)
	: wxComboBox(parent, wxID_ANY, value, wxDefaultPosition, wxSize(220, 44), choices, style) {
		SetMinSize(wxSize(220, 44));
		CenterNativeComboBoxText(this);
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			CenterNativeComboBoxText(this);
			event.Skip();
		});
	}
};

class TestFrame final : public wxFrame {
public:
	TestFrame()
	: wxFrame(nullptr, wxID_ANY, "wxComboBox alignment test", wxDefaultPosition, wxSize(720, 560)) {
		auto panel = new wxPanel(this);
		auto root = new wxBoxSizer(wxVERTICAL);

		wxArrayString choices;
		choices.Add("25%");
		choices.Add("50%");
		choices.Add("75%");
		choices.Add("100%");
		choices.Add("1.0x");
		choices.Add("1.25x");

		root->Add(new wxStaticText(panel, wxID_ANY, "Native wxComboBox"), 0, wxLEFT | wxRIGHT | wxTOP, 20);
		auto native = new wxComboBox(panel, wxID_ANY, "50%", wxDefaultPosition, wxSize(220, 44), choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
		native->SetMinSize(wxSize(220, 44));
		root->Add(native, 0, wxLEFT | wxRIGHT | wxTOP, 20);

		root->Add(new wxStaticText(panel, wxID_ANY, "Centered custom wxComboBox"), 0, wxLEFT | wxRIGHT | wxTOP, 20);
		root->Add(new CenteredComboBox(panel, "50%", choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER), 0, wxLEFT | wxRIGHT | wxTOP, 20);

		root->Add(new wxStaticText(panel, wxID_ANY, "Centered read-only wxComboBox"), 0, wxLEFT | wxRIGHT | wxTOP, 20);
		root->Add(new CenteredComboBox(panel, "1.0x", choices, wxCB_READONLY | wxCB_DROPDOWN), 0, wxLEFT | wxRIGHT | wxTOP, 20);

		wxArrayString zoomChoices;
		for (int i = 1; i <= 24; ++i)
			zoomChoices.Add(wxString::Format("%g%%", i * 12.5));

		wxArrayString speedChoices;
		speedChoices.Add("0.25x");
		speedChoices.Add("0.5x");
		speedChoices.Add("0.75x");
		speedChoices.Add("1.0x");
		speedChoices.Add("1.25x");
		speedChoices.Add("1.5x");
		speedChoices.Add("2.0x");

		root->Add(new wxStaticText(panel, wxID_ANY, "Aegisub-like video controls, native"), 0, wxLEFT | wxRIGHT | wxTOP, 20);
		auto nativeRow = new wxBoxSizer(wxHORIZONTAL);
		nativeRow->Add(new wxButton(panel, wxID_ANY, "▶", wxDefaultPosition, wxSize(42, -1)), wxSizerFlags(0).Center());
		nativeRow->Add(new wxTextCtrl(panel, wxID_ANY, "0:00:00.000 - 0", wxDefaultPosition, wxSize(180, -1), wxTE_READONLY), wxSizerFlags(1).Center().Border(wxLEFT));
		nativeRow->Add(new wxTextCtrl(panel, wxID_ANY, "-132240ms; -133180ms", wxDefaultPosition, wxSize(220, -1), wxTE_READONLY), wxSizerFlags(1).Center().Border(wxLEFT));
		nativeRow->Add(MakeAegisubCombo(panel, "50%", "75%", zoomChoices, false), wxSizerFlags(0).Center().Border(wxLEFT | wxRIGHT));
		nativeRow->Add(MakeAegisubCombo(panel, "1.0x", "10.0x", speedChoices, false), wxSizerFlags(0).Center().Border(wxRIGHT));
		root->Add(nativeRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 20);

		root->Add(new wxStaticText(panel, wxID_ANY, "Aegisub-like video controls, centered native cell"), 0, wxLEFT | wxRIGHT | wxTOP, 20);
		auto centeredRow = new wxBoxSizer(wxHORIZONTAL);
		centeredRow->Add(new wxButton(panel, wxID_ANY, "▶", wxDefaultPosition, wxSize(42, -1)), wxSizerFlags(0).Center());
		centeredRow->Add(new wxTextCtrl(panel, wxID_ANY, "0:00:00.000 - 0", wxDefaultPosition, wxSize(180, -1), wxTE_READONLY), wxSizerFlags(1).Center().Border(wxLEFT));
		centeredRow->Add(new wxTextCtrl(panel, wxID_ANY, "-132240ms; -133180ms", wxDefaultPosition, wxSize(220, -1), wxTE_READONLY), wxSizerFlags(1).Center().Border(wxLEFT));
		centeredRow->Add(MakeAegisubCombo(panel, "50%", "75%", zoomChoices, true), wxSizerFlags(0).Center().Border(wxLEFT | wxRIGHT));
		centeredRow->Add(MakeAegisubCombo(panel, "1.0x", "10.0x", speedChoices, true), wxSizerFlags(0).Center().Border(wxRIGHT));
		root->Add(centeredRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 20);

		panel->SetSizer(root);
		Centre();
	}
};

class TestApp final : public wxApp {
public:
	bool OnInit() override {
		auto frame = new TestFrame;
		frame->Show();
		return true;
	}
};

wxIMPLEMENT_APP(TestApp);
