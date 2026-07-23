/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ShortcutKeys.h"

#if !0 && !0 && \
    !defined(MOZ_WIDGET_GTK) && !0
#  define USE_EMACS_KEY_BINDINGS
#endif


namespace mozilla {

ShortcutKeyData ShortcutKeys::sInputHandlers[] = {
// clang-format off
#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, nullptr,  u"cmd_moveLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, nullptr,  u"cmd_moveRight"},  
    {u"keypress", u"VK_UP",    nullptr, nullptr,  u"cmd_moveUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, nullptr,  u"cmd_moveDown"},   
#endif

#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"shift", u"cmd_selectLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, u"shift", u"cmd_selectRight"},  
    {u"keypress", u"VK_UP",    nullptr, u"shift", u"cmd_selectUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, u"shift", u"cmd_selectDown"},   
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"control",       u"cmd_wordPrevious"},        
    {u"keypress", u"VK_RIGHT", nullptr, u"control",       u"cmd_wordNext"},            
    {u"keypress", u"VK_LEFT",  nullptr, u"shift,control", u"cmd_selectWordPrevious"},  
    {u"keypress", u"VK_RIGHT", nullptr, u"shift,control", u"cmd_selectWordNext"},      
#endif



#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_HOME", nullptr, nullptr,          u"cmd_beginLine"},        
    {u"keypress", u"VK_END",  nullptr, nullptr,          u"cmd_endLine"},          
    {u"keypress", u"VK_HOME", nullptr, u"shift",         u"cmd_selectBeginLine"},  
    {u"keypress", u"VK_END",  nullptr, u"shift",         u"cmd_selectEndLine"},    
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_HOME", nullptr, u"control",       u"cmd_beginLine"},        
    {u"keypress", u"VK_END",  nullptr, u"control",       u"cmd_endLine"},          
    {u"keypress", u"VK_HOME", nullptr, u"control,shift", u"cmd_selectBeginLine"},  
    {u"keypress", u"VK_END",  nullptr, u"control,shift", u"cmd_selectEndLine"},    
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_INSERT", nullptr, u"control", u"cmd_copy"},   
    {u"keypress", u"VK_INSERT", nullptr, u"shift",   u"cmd_paste"},  
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"shift",   u"cmd_cutOrDelete"},        
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"control", u"cmd_copyOrDelete"},       
#endif

#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_BACK", nullptr, u"control",   u"cmd_deleteWordBackward"},       
#endif

    {u"keypress", nullptr, u"c", u"accel",       u"cmd_copy"},   
    {u"keypress", nullptr, u"x", u"accel",       u"cmd_cut"},    
    {u"keypress", nullptr, u"v", u"accel",       u"cmd_paste"},  
    {u"keypress", nullptr, u"z", u"accel",       u"cmd_undo"},   
    {u"keypress", nullptr, u"z", u"accel,shift", u"cmd_redo"},   

    {u"keypress", nullptr, u"v", u"accel,shift",     u"cmd_paste"},  

#if 0 || defined(MOZ_WIDGET_GTK) ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"y", u"accel",       u"cmd_redo"},   
#endif

#if 0 || 0 || defined(MOZ_WIDGET_GTK) ||\
    0
    {u"keypress", nullptr, u"a", u"accel",       u"cmd_selectAll"},  
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"alt",         u"cmd_selectAll"},  
#endif

#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"control", u"cmd_beginLine"},                
    {u"keypress", nullptr, u"e", u"control", u"cmd_endLine"},                  
    {u"keypress", nullptr, u"b", u"control", u"cmd_charPrevious"},             
    {u"keypress", nullptr, u"f", u"control", u"cmd_charNext"},                 
    {u"keypress", nullptr, u"h", u"control", u"cmd_deleteCharBackward"},       
    {u"keypress", nullptr, u"d", u"control", u"cmd_deleteCharForward"},        
    {u"keypress", nullptr, u"w", u"control", u"cmd_deleteWordBackward"},       
    {u"keypress", nullptr, u"u", u"control", u"cmd_deleteToBeginningOfLine"},  
    {u"keypress", nullptr, u"k", u"control", u"cmd_deleteToEndOfLine"},        
#endif
    // clang-format on

    {nullptr, nullptr, nullptr, nullptr, nullptr}};

ShortcutKeyData ShortcutKeys::sTextAreaHandlers[] = {
// clang-format off
#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, nullptr, u"cmd_moveLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, nullptr, u"cmd_moveRight"},  
    {u"keypress", u"VK_UP",    nullptr, nullptr, u"cmd_moveUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, nullptr, u"cmd_moveDown"},   
#endif

#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"shift", u"cmd_selectLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, u"shift", u"cmd_selectRight"},  
    {u"keypress", u"VK_UP",    nullptr, u"shift", u"cmd_selectUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, u"shift", u"cmd_selectDown"},   
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"control",       u"cmd_wordPrevious"},        
    {u"keypress", u"VK_RIGHT", nullptr, u"control",       u"cmd_wordNext"},            
    {u"keypress", u"VK_LEFT",  nullptr, u"shift,control", u"cmd_selectWordPrevious"},  
    {u"keypress", u"VK_RIGHT", nullptr, u"shift,control", u"cmd_selectWordNext"},      
#endif



#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_PAGE_UP",   nullptr, nullptr,      u"cmd_movePageUp"},      
    {u"keypress", u"VK_PAGE_DOWN", nullptr, nullptr,      u"cmd_movePageDown"},    
    {u"keypress", u"VK_PAGE_UP",   nullptr, u"shift",     u"cmd_selectPageUp"},    
    {u"keypress", u"VK_PAGE_DOWN", nullptr, u"shift",     u"cmd_selectPageDown"},  
#endif

#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_HOME",      nullptr, nullptr,           u"cmd_beginLine"},                
    {u"keypress", u"VK_END",       nullptr, nullptr,           u"cmd_endLine"},                  
    {u"keypress", u"VK_HOME",      nullptr, u"shift",          u"cmd_selectBeginLine"},          
    {u"keypress", u"VK_END",       nullptr, u"shift",          u"cmd_selectEndLine"},            
    {u"keypress", u"VK_HOME",      nullptr, u"control",        u"cmd_moveTop"},                  
    {u"keypress", u"VK_END",       nullptr, u"control",        u"cmd_moveBottom"},               
    {u"keypress", u"VK_HOME",      nullptr, u"shift,control",  u"cmd_selectTop"},                
    {u"keypress", u"VK_END",       nullptr, u"shift,control",  u"cmd_selectBottom"},             
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_INSERT", nullptr, u"control", u"cmd_copy"},   
    {u"keypress", u"VK_INSERT", nullptr, u"shift",   u"cmd_paste"},  
#endif

    {u"keypress", nullptr, u"v", u"accel,shift",     u"cmd_paste"},  

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"shift",   u"cmd_cutOrDelete"},        
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"control", u"cmd_copyOrDelete"},       
#endif

#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_BACK", nullptr, u"control",   u"cmd_deleteWordBackward"},       
#endif

    {u"keypress", nullptr, u"c", u"accel",       u"cmd_copy"},       
    {u"keypress", nullptr, u"x", u"accel",       u"cmd_cut"},        
    {u"keypress", nullptr, u"v", u"accel",       u"cmd_paste"},      
    {u"keypress", nullptr, u"z", u"accel",       u"cmd_undo"},       
    {u"keypress", nullptr, u"z", u"accel,shift", u"cmd_redo"},       

#if 0 || defined(MOZ_WIDGET_GTK) ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"y", u"accel",       u"cmd_redo"},       
#endif

#if 0 || 0 || defined(MOZ_WIDGET_GTK) ||\
    0
    {u"keypress", nullptr, u"a", u"accel",       u"cmd_selectAll"},  
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"alt",         u"cmd_selectAll"},  
#endif

#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"control",     u"cmd_beginLine"},                
    {u"keypress", nullptr, u"e", u"control",     u"cmd_endLine"},                  
    {u"keypress", nullptr, u"b", u"control",     u"cmd_charPrevious"},             
    {u"keypress", nullptr, u"f", u"control",     u"cmd_charNext"},                 
    {u"keypress", nullptr, u"h", u"control",     u"cmd_deleteCharBackward"},       
    {u"keypress", nullptr, u"d", u"control",     u"cmd_deleteCharForward"},        
    {u"keypress", nullptr, u"w", u"control",     u"cmd_deleteWordBackward"},       
    {u"keypress", nullptr, u"u", u"control",     u"cmd_deleteToBeginningOfLine"},  
    {u"keypress", nullptr, u"k", u"control",     u"cmd_deleteToEndOfLine"},        
    {u"keypress", nullptr, u"n", u"control",     u"cmd_lineNext"},                 
    {u"keypress", nullptr, u"p", u"control",     u"cmd_linePrevious"},             
#endif
    // clang-format on

    {nullptr, nullptr, nullptr, nullptr, nullptr}};

ShortcutKeyData ShortcutKeys::sBrowserHandlers[] = {
    // clang-format off
    {u"keypress", u"VK_LEFT",  nullptr, nullptr,  u"cmd_moveLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, nullptr,  u"cmd_moveRight"},  
    {u"keypress", u"VK_UP",    nullptr, nullptr,  u"cmd_moveUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, nullptr,  u"cmd_moveDown"},   

#if 0 || 0 || defined(MOZ_WIDGET_GTK)
    {u"keypress", u"VK_LEFT",  nullptr, u"shift", u"cmd_selectLeft"},          
    {u"keypress", u"VK_RIGHT", nullptr, u"shift", u"cmd_selectRight"},         
    {u"keypress", u"VK_UP",    nullptr, u"shift", u"cmd_selectUp"},            
    {u"keypress", u"VK_DOWN",  nullptr, u"shift", u"cmd_selectDown"},          
#endif
#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"shift", u"cmd_selectCharPrevious"},  
    {u"keypress", u"VK_RIGHT", nullptr, u"shift", u"cmd_selectCharNext"},      
    {u"keypress", u"VK_UP",    nullptr, u"shift", u"cmd_selectLinePrevious"},  
    {u"keypress", u"VK_DOWN",  nullptr, u"shift", u"cmd_selectLineNext"},      
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"control",       u"cmd_wordPrevious"},        
    {u"keypress", u"VK_RIGHT", nullptr, u"control",       u"cmd_wordNext"},            
    {u"keypress", u"VK_LEFT",  nullptr, u"control,shift", u"cmd_selectWordPrevious"},  
    {u"keypress", u"VK_RIGHT", nullptr, u"control,shift", u"cmd_selectWordNext"},      
#endif
#if 0 || defined(MOZ_WIDGET_GTK)
    {u"keypress", u"VK_LEFT",  nullptr, u"control",       u"cmd_moveLeft2"},           
    {u"keypress", u"VK_RIGHT", nullptr, u"control",       u"cmd_moveRight2"},          
    {u"keypress", u"VK_LEFT",  nullptr, u"control,shift", u"cmd_selectLeft2"},         
    {u"keypress", u"VK_RIGHT", nullptr, u"control,shift", u"cmd_selectRight2"},        
#endif

#if 0 || defined(MOZ_WIDGET_GTK)
    {u"keypress", u"VK_UP",   nullptr, u"control",       u"cmd_moveUp2"},       
    {u"keypress", u"VK_DOWN", nullptr, u"control",       u"cmd_moveDown2"},     
    {u"keypress", u"VK_UP",   nullptr, u"control,shift", u"cmd_selectUp2"},     
    {u"keypress", u"VK_DOWN", nullptr, u"control,shift", u"cmd_selectDown2"},   
#endif


#if 0 || defined(MOZ_WIDGET_GTK) ||\
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_PAGE_UP",   nullptr, nullptr,      u"cmd_movePageUp"},      
    {u"keypress", u"VK_PAGE_DOWN", nullptr, nullptr,      u"cmd_movePageDown"},    
    {u"keypress", u"VK_PAGE_UP",   nullptr, u"shift",     u"cmd_selectPageUp"},    
    {u"keypress", u"VK_PAGE_DOWN", nullptr, u"shift",     u"cmd_selectPageDown"},  
#endif

#if 0 || defined(MOZ_WIDGET_GTK) ||\
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_HOME", nullptr, nullptr,          u"cmd_beginLine"},        
    {u"keypress", u"VK_END",  nullptr, nullptr,          u"cmd_endLine"},          
    {u"keypress", u"VK_HOME", nullptr, u"shift",         u"cmd_selectBeginLine"},  
    {u"keypress", u"VK_END",  nullptr, u"shift",         u"cmd_selectEndLine"},    
    {u"keypress", u"VK_HOME", nullptr, u"control",       u"cmd_moveTop"},          
    {u"keypress", u"VK_END",  nullptr, u"control",       u"cmd_moveBottom"},       
    {u"keypress", u"VK_HOME", nullptr, u"shift,control", u"cmd_selectTop"},        
    {u"keypress", u"VK_END",  nullptr, u"shift,control", u"cmd_selectBottom"},     
#endif

#if 0 || defined(MOZ_WIDGET_GTK) ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_INSERT",    nullptr, u"control",        u"cmd_copy"},  
#endif

#if 0 || defined(MOZ_WIDGET_GTK) ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"shift",   u"cmd_cut"},                
#endif
#if defined(MOZ_WIDGET_GTK) || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"control", u"cmd_copy"},               
#endif


    {u"keypress", nullptr, u"c", u"accel",       u"cmd_copy"},              
    {u"keypress", nullptr, u"x", u"accel",       u"cmd_cut"},               
    {u"keypress", nullptr, u"v", u"accel",       u"cmd_paste"},             
    {u"keypress", nullptr, u"v", u"accel,shift", u"cmd_pasteNoFormatting"}, 
    {u"keypress", nullptr, u"z", u"accel",       u"cmd_undo"},              
    {u"keypress", nullptr, u"z", u"accel,shift", u"cmd_redo"},              



    {u"keypress", nullptr, u"a", u"accel",       u"cmd_selectAll"},  
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"alt",         u"cmd_selectAll"},  
#endif

    {u"keypress", nullptr, u" ", nullptr,  u"cmd_scrollPageDown"},  
    {u"keypress", nullptr, u" ", u"shift", u"cmd_scrollPageUp"},    


    {nullptr, nullptr, nullptr, nullptr, nullptr}};

ShortcutKeyData ShortcutKeys::sEditorHandlers[] = {
// clang-format off
#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, nullptr,  u"cmd_moveLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, nullptr,  u"cmd_moveRight"},  
    {u"keypress", u"VK_UP",    nullptr, nullptr,  u"cmd_moveUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, nullptr,  u"cmd_moveDown"},   
#endif

#if 0 || defined(MOZ_WIDGET_GTK) || \
    0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"shift", u"cmd_selectLeft"},   
    {u"keypress", u"VK_RIGHT", nullptr, u"shift", u"cmd_selectRight"},  
    {u"keypress", u"VK_UP",    nullptr, u"shift", u"cmd_selectUp"},     
    {u"keypress", u"VK_DOWN",  nullptr, u"shift", u"cmd_selectDown"},   
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_LEFT",  nullptr, u"control",       u"cmd_wordPrevious"},        
    {u"keypress", u"VK_RIGHT", nullptr, u"control",       u"cmd_wordNext"},            
    {u"keypress", u"VK_LEFT",  nullptr, u"shift,control", u"cmd_selectWordPrevious"},  
    {u"keypress", u"VK_RIGHT", nullptr, u"shift,control", u"cmd_selectWordNext"},      
#endif



#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_PAGE_UP",   nullptr, nullptr,      u"cmd_movePageUp"},      
    {u"keypress", u"VK_PAGE_DOWN", nullptr, nullptr,      u"cmd_movePageDown"},    
    {u"keypress", u"VK_PAGE_UP",   nullptr, u"shift",     u"cmd_selectPageUp"},    
    {u"keypress", u"VK_PAGE_DOWN", nullptr, u"shift",     u"cmd_selectPageDown"},  
#endif

#if 0 || 0 ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_HOME", nullptr, nullptr,          u"cmd_beginLine"},        
    {u"keypress", u"VK_END",  nullptr, nullptr,          u"cmd_endLine"},          
    {u"keypress", u"VK_HOME", nullptr, u"shift",         u"cmd_selectBeginLine"},  
    {u"keypress", u"VK_END",  nullptr, u"shift",         u"cmd_selectEndLine"},    
    {u"keypress", u"VK_HOME", nullptr, u"control",       u"cmd_moveTop"},          
    {u"keypress", u"VK_END",  nullptr, u"control",       u"cmd_moveBottom"},       
    {u"keypress", u"VK_HOME", nullptr, u"shift,control", u"cmd_selectTop"},        
    {u"keypress", u"VK_END",  nullptr, u"shift,control", u"cmd_selectBottom"},     
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_INSERT", nullptr, u"control", u"cmd_copy"},   
    {u"keypress", u"VK_INSERT", nullptr, u"shift",   u"cmd_paste"},  
#endif

#if 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"shift",   u"cmd_cutOrDelete"},        
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_DELETE", nullptr, u"control", u"cmd_copyOrDelete"},       
#endif

#if 0 || 0 || defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", u"VK_BACK", nullptr, u"control",   u"cmd_deleteWordBackward"},       
#endif

    {u"keypress", nullptr, u"c", u"accel",           u"cmd_copy"},               
    {u"keypress", nullptr, u"x", u"accel",           u"cmd_cut"},                
    {u"keypress", nullptr, u"v", u"accel",           u"cmd_paste"},              
    {u"keypress", nullptr, u"v", u"accel,shift",     u"cmd_pasteNoFormatting"},  
    {u"keypress", nullptr, u"z", u"accel",           u"cmd_undo"},               
    {u"keypress", nullptr, u"z", u"accel,shift",     u"cmd_redo"},               


#if 0 || defined(MOZ_WIDGET_GTK) ||\
    defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"y", u"accel",           u"cmd_redo"},               
#endif

#if 0 || 0 || defined(MOZ_WIDGET_GTK) ||\
    0
    {u"keypress", nullptr, u"a", u"accel",           u"cmd_selectAll"},          
#endif
#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"a", u"alt",             u"cmd_selectAll"},          
#endif

    {u"keypress", nullptr, u" ", nullptr,  u"cmd_scrollPageDown"},  
    {u"keypress", nullptr, u" ", u"shift", u"cmd_scrollPageUp"},    

#if defined(USE_EMACS_KEY_BINDINGS)
    {u"keypress", nullptr, u"h", u"control", u"cmd_deleteCharBackward"},       
    {u"keypress", nullptr, u"d", u"control", u"cmd_deleteCharForward"},        
    {u"keypress", nullptr, u"k", u"control", u"cmd_deleteToEndOfLine"},        
    {u"keypress", nullptr, u"u", u"control", u"cmd_deleteToBeginningOfLine"},  
    {u"keypress", nullptr, u"a", u"control", u"cmd_beginLine"},                
    {u"keypress", nullptr, u"e", u"control", u"cmd_endLine"},                  
    {u"keypress", nullptr, u"b", u"control", u"cmd_charPrevious"},             
    {u"keypress", nullptr, u"f", u"control", u"cmd_charNext"},                 
    {u"keypress", nullptr, u"p", u"control", u"cmd_linePrevious"},             
    {u"keypress", nullptr, u"n", u"control", u"cmd_lineNext"},                 
#endif
    // clang-format on

    {nullptr, nullptr, nullptr, nullptr, nullptr}};

}  

#undef USE_EMACS_KEY_BINDINGS
