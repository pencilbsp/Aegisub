// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#import <AppKit/AppKit.h>

#include <wx/event.h>
#include <wx/osx/private.h>
#include <wx/stc/stc.h>

namespace {

NSView *GetTextInputView(wxStyledTextCtrl *ctrl) {
	if (!ctrl)
		return nil;

	NSView *view = (NSView *)ctrl->GetHandle();
	if (![view isKindOfClass:[NSView class]])
		return nil;

	if ([view isKindOfClass:[NSScrollView class]]) {
		NSView *document_view = [(NSScrollView *)view documentView];
		if (document_view)
			view = document_view;
	}

	return view;
}

bool HasMarkedText(NSView *view) {
	return view && [view respondsToSelector:@selector(hasMarkedText)] && [(id<NSTextInputClient>)view hasMarkedText];
}

}

namespace osx { namespace ime {

void inject(wxStyledTextCtrl *) {
}

void invalidate(wxStyledTextCtrl *ctrl) {
	NSView *view = GetTextInputView(ctrl);
	if (!view)
		return;

	if ([view respondsToSelector:@selector(inputContext)])
		[[view inputContext] discardMarkedText];

	if ([view respondsToSelector:@selector(unmarkText)])
		[(id<NSTextInputClient>)view unmarkText];
}

bool process_key_event(wxStyledTextCtrl *ctrl, wxKeyEvent &evt) {
	if (evt.GetModifiers() != 0)
		return false;

	if (evt.GetKeyCode() != WXK_RETURN && evt.GetKeyCode() != WXK_TAB)
		return false;

	if (!HasMarkedText(GetTextInputView(ctrl)))
		return false;

	evt.Skip();
	return true;
}

} }
