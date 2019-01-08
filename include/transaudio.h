/*****************************************************************************                                                                                                                          
  Copyright (C) 2018 Fillet                                                                                                                                                                              
                                                                                                                                                                                                           
  This program is free software; you can redistribute it and/or modify                                                                                                                                     
  it under the terms of the GNU General Public License as published by                                                                                                                                     
  the Free Software Foundation; either version 2 of the License, or                                                                                                                                        
  (at your option) any later version.                                                                                                                                                                      
                                                                                                                                                                                                           
  This program is distributed in the hope that it will be useful,                                                                                                                                          
  but WITHOUT ANY WARRANTY; without even the implied warranty of                                                                                                                                           
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                                                                                                                                            
  GNU General Public License for more details.                                                                                                                                                             
                                                                                                                                                                                                           
  You should have received a copy of the GNU General Public License                                                                                                                                        
  along with this program; if not, write to the Free Software                                                                                                                                              
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111, USA.                                                                                                                                
                                                                                                                                                                                                           
  This program is also available under a commercial license with                                                                                                                                           
  customization/support packages and additional features.  For more                                                                                                                                        
  information, please contact us at cannonbeachgoonie@gmail.com                                                                                                                                          
                                                                                                                                                                                                           
******************************************************************************/

#if !defined(_TRANSAUDIO_H_)
#define _TRANSAUDIO_H_

#include "fillet.h"

int start_audio_transcode_threads(fillet_app_struct *core);
int stop_audio_transcode_threads(fillet_app_struct *core);

#endif // _TRANSAUDIO_H_
