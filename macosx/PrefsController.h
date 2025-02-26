// This file Copyright © 2005-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "PortChecker.h"
#import <AppKit/AppKit.h>

#include <libtransmission/transmission.h>

@interface PrefsController : NSWindowController<NSToolbarDelegate, PortCheckerDelegate>

@property(nonatomic, readonly) NSArray<NSString*>* sounds;

+ (NSInteger)dateToTimeSum:(NSDate*)date;

- (instancetype)initWithHandle:(tr_session*)handle;

- (void)rpcUpdatePrefs;

@end
