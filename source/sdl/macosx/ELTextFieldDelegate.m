// Emacs style mode select -*- Objective-C -*-
//----------------------------------------------------------------------------
//
// Copyright(C) 2012 Ioan Chera
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//----------------------------------------------------------------------------
//
// DESCRIPTION:
//
// Instant edit action responding delegate for real time parameter updating
//
//----------------------------------------------------------------------------

#import "ELTextFieldDelegate.h"
#import "LauncherController.h"

@implementation ELTextFieldDelegate

//
// dealloc
//
-(void)dealloc
{
   
   // Remove observer
   [[NSNotificationCenter defaultCenter] removeObserver:self];
   
   [super dealloc];
}

//
// control:textShouldBeginEditing:
//
-(void)textDidChange:(NSNotification *)aNotification
{
   [owner updateParameters:[aNotification object]];
}

@end

// EOF

