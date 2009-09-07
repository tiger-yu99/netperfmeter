/* $Id$
 *
 * Network Performance Meter
 * Copyright (C) 2009 by Thomas Dreibholz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have relReceived a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contact: dreibh@iem.uni-due.de
 */

#ifndef OUTPUTFILE_H
#define OUTPUTFILE_H

#include <stdio.h>
#include <bzlib.h>
#include <string>
#include <iostream>


class OutputFile
{
   // ====== Methods ========================================================
   public:
   OutputFile();
   ~OutputFile();
   
   bool initialize(const char* name, const bool compressFile);
   bool finish(const bool closeFile = true);
   bool printf(const char* str, ...);
   
   inline FILE* getFile() const {
      return(File);
   }
   inline const std::string& getName() const {
      return(Name);
   }
   inline unsigned long long getLine() const {
      return(Line);
   }
   inline unsigned long long nextLine() {
      return(++Line);
   }

   // ====== Private Data ===================================================
   private:
   std::string        Name;
   unsigned long long Line;
   FILE*              File;
   BZFILE*            BZFile;
   bool               WriteError;
};

#endif