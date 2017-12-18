/*
This file is part of the HemoCell library

HemoCell is developed and maintained by the Computational Science Lab 
in the University of Amsterdam. Any questions or remarks regarding this library 
can be sent to: info@hemocell.eu

When using the HemoCell library in scientific work please cite the
corresponding paper: https://doi.org/10.3389/fphys.2017.00563

The HemoCell library is free software: you can redistribute it and/or
modify it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

The library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"
#include "genericFunctions.h"
#include <stdexcept>
#include <sys/stat.h>

namespace hemo {
  Config::Config(string paramXmlFileName) 
  {  
    if (!file_exists(paramXmlFileName)) {
      pcout << paramXmlFileName + " is not an existing config file, exiting ..." << endl;
      exit(0);
    }
    orig = new tinyxml2::XMLDocument();
    orig->LoadFile(paramXmlFileName.c_str());
   // Check if it is a fresh start or a checkpointed run 
    tinyxml2::XMLNode * first =orig->FirstChild()->NextSibling();
    if (first) {
      string firstField = first->Value(); 
      if (firstField == "Checkpoint") { checkpointed = 1; } //If the first field is not checkpoint but hemocell
      else { checkpointed = 0; }
    }
  }

  /*
   * Overload the overloaded operator to provide convenient access to
   * this["parameters"]["etc"]
   * Also hide the read() semantics, so it either returns a 
   */
  XMLElement  Config::operator[] (string name) const
  {
   // Set our direct access
   if (checkpointed) {
     return static_cast<XMLElement>(orig->FirstChildElement("Checkpoint"))["hemocell"][name];
   } else {
     return static_cast<XMLElement>(orig->FirstChildElement("hemocell"))[name];
   }
  }
  
  XMLElement XMLElement::operator[] (string name) const {
    tinyxml2::XMLElement * child = orig->FirstChildElement(name.c_str());
    if (child) {
      return static_cast<XMLElement>(child);
    } else {
      throw invalid_argument("XML child " + name + " does not exist.");
    }
  }

  
}
