// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 2000-2011, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*   file name:  ucol_data.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2011jul02
*   created by: Markus Scherer
*
* Private implementation header for C/C++ collation.
* Some file data structure definitions were moved here from i18n/ucol_imp.h
* so that the common library (via ucol_swp.cpp) need not depend on the i18n library at all.
*
* We do not want to move the collation swapper to the i18n library because
* a) the resource bundle swapper depends on it and would have to move too, and
* b) we might want to eventually implement runtime data swapping,
*    which might (or might not) be easier if all swappers are in the common library.
*/

#ifndef __UCOL_DATA_H__
#define __UCOL_DATA_H__

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION

#define UCOL_HEADER_MAGIC 0x20030618

typedef struct {
      int32_t size;
      uint32_t options; 
      uint32_t UCAConsts; 
      uint32_t contractionUCACombos;        
      uint32_t magic;            
      uint32_t mappingPosition;  
      uint32_t expansion;        
      uint32_t contractionIndex; 
      uint32_t contractionCEs;   
      uint32_t contractionSize;  
       

      uint32_t endExpansionCE;      
      uint32_t expansionCESize;     
      int32_t  endExpansionCECount; 
      uint32_t unsafeCP;            
      uint32_t contrEndCP;          

      int32_t contractionUCACombosSize;     
      UBool jamoSpecial;                    
      UBool isBigEndian;                    
      uint8_t charSetFamily;                
      uint8_t contractionUCACombosWidth;    
      UVersionInfo version;
      UVersionInfo UCAVersion;              
      UVersionInfo UCDVersion;              
      UVersionInfo formatVersion;           
      uint32_t scriptToLeadByte;            
      uint32_t leadByteToScript;            
      uint8_t reserved[76];                 
} UCATableHeader;

typedef struct {
  uint32_t byteSize;
  uint32_t tableSize;
  uint32_t contsSize;
  uint32_t table;
  uint32_t conts;
  UVersionInfo UCAVersion;              
  uint8_t padding[8];
} InverseUCATableHeader;

#endif  /* !UCONFIG_NO_COLLATION */

#endif  /* __UCOL_DATA_H__ */
