/*
---------------------------------------------------------------------------

    This is an extension of uHAL to directly access AXI slaves via the linux
    UIO driver. 

    This file is part of uHAL.

    uHAL is a hardware access library and programming framework
    originally developed for upgrades of the Level-1 trigger of the CMS
    experiment at CERN.

    uHAL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    uHAL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with uHAL.  If not, see <http://www.gnu.org/licenses/>.


      Andrew Rose, Imperial College, London
      email: awr01 <AT> imperial.ac.uk

      Marc Magrans de Abril, CERN
      email: marc.magrans.de.abril <AT> cern.ch

      Tom Williams, Rutherford Appleton Laboratory, Oxfordshire
      email: tom.williams <AT> cern.ch

      Dan Gastler, Boston University 
      email: dgastler <AT> bu.edu
      
---------------------------------------------------------------------------
*/
/**
	@file
	@author Siqi Yuan / Dan Gastler / Theron Jasper Tarigo
*/

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <boost/filesystem.hpp>
#include <boost/shared_ptr.hpp>
#include <uhal/Node.hpp>
#include <uhal/NodeTreeBuilder.hpp>
#include <pugixml.hpp>
#include "uhal/log/LogLevels.hpp"
#include "uhal/log/log_inserters.integer.hpp"
#include "uhal/log/log.hpp"

#include <uhal/ProtocolUIOPCI.hpp>


#include <fstream>

using namespace uiopci;
using namespace boost::filesystem;



namespace uhal {  

  UIOPCI::UIOPCI (
	    const std::string& aId, const URI& aUri,
	    const boost::posix_time::time_duration&aTimeoutPeriod
	    ) :
    ClientInterface(aId,aUri,aTimeoutPeriod)
  { 
    for (int i=0; i<uiopci::DEVICES_MAX ; i++){ fd[i]=-1; hw[i]=NULL; }
    // Get the filename of the address table from the connection file. Then read it through the NodeTreeBuilder
    // The NodeTreeBuilder should be able to just use the existing node tree rather than rebuild a new one
    NodeTreeBuilder & mynodetreebuilder = NodeTreeBuilder::getInstance();
    //boost::shared_ptr< Node > lNode ( mynodetreebuilder.getNodeTree ( tabfname , boost::filesystem::current_path() / "." ) );
    Node* lNode = ( mynodetreebuilder.getNodeTree ( std::string("file://")+aUri.mHostname , boost::filesystem::current_path() / "." ) );
    // Getting the IDs for only first layer nodes (nodes that contain device labels). matching names that doesn't contain a "."
    std::vector< std::string > top_node_Ids = lNode->getNodes("^[^.]+$");
    // For each device label, search for its matching device
    for (std::vector<std::string>::iterator nodeId = top_node_Ids.begin(); nodeId != top_node_Ids.end(); ++nodeId) {
      // device number is the number read from the most significant 8 bits of the address
      // size should be read from /sys/class/uio*/maps/map0/size
      int devnum=-1, size=0;
      char label[128]="", sysfsResourceName[128]=""; 
      // get the device number out from the node
      devnum = decodeAddress(lNode->getNode(*nodeId).getAddress()).device;
      // search through the file system to see if there is a uio that matches the name
      std::string uiopath = "/sys/class/uio/";
      FILE *labelfile=0; 
      // traverse through the UIO devices
      for (directory_iterator x(uiopath); x!=directory_iterator(); ++x){
	//Find the foo/device/device file

	if (!is_directory(x->path())) {
	  continue;
	}
	if (!exists(x->path()/"device")) {
	  continue;
	}
	labelfile = fopen((x->path().native()+"/device").c_str(),"r");
	label[0] = 'P';
	label[1] = 'C';
	label[2] = 'I';
	label[3] = '_';
	fgets(label+4,128,labelfile); fclose(labelfile);
	if(!strcmp(label, (*nodeId).c_str())){  
	  //Open the resource file and subtract the first two lines to get the memory size
	  std::ifstream inSizeFile((x->path().native()+"resource").c_str(),std::ifstream::in);
	  uint64_t memStart,memEnd;
	  inSizeFile >> memStart ;
	  inSizeFile >> memEnd;
	  size = (memEnd - memStart + 1)/4;

	  //save the name of the resource file for this. 
	  strcpy(sysfsResourceName,(x->path().native()+"/resource0").c_str());
	  break;
	}      
      }
      //save the mapping
      strcpy(uionames[devnum],sysfsResourceName);
      openDevice(devnum, size,sysfsResourceName);
    }

  
  }

  UIOPCI::~UIOPCI () {
    log ( Debug() , "UIOPCI: destructor" );
    sigaction(SIGBUS,&saBusError_old,NULL); //restore the signal handler from before creation for SIGBUS
  }

  void
  UIOPCI::openDevice(int i, uint32_t size, const char *name) {
    if (i<0||i>=DEVICES_MAX) return;
    const char *prefix = "/dev";
    size_t devpath_cap = strlen(prefix)+1+strlen(name)+1;
    char *devpath = (char*)malloc(devpath_cap);
    snprintf(devpath,devpath_cap, "%s/%s", prefix, name);
    fd[i] = open(devpath, O_RDWR|O_SYNC);
    if (-1==fd[i]) {
      log( Debug() , "Failed to open ", devpath, ": ", strerror(errno));
      goto end;
    }
    hw[i] = (uint32_t*)mmap( NULL, size*sizeof(uint32_t),
			     PROT_READ|PROT_WRITE, MAP_SHARED,
			     fd[i], 0x0);
    if (hw[i]==MAP_FAILED) {
      log ( Debug() , "Failed to map ", devpath, ": ",  strerror(errno));
      hw[i]=NULL;
      goto end;
    }
    log ( Debug(), "Mapped ", devpath, " as device number ", Integer( i, IntFmt<hex,fixed>()),
	  " size ", Integer( size, IntFmt<hex, fixed>()));
  end:
    free(devpath);
  }

  int
  UIOPCI::checkDevice (int i) {
    if (!hw[i]) {
      // Todo: replace with an exception
      uhal::exception::BadUIOPCIDevice* lExc = new uhal::exception::BadUIOPCIDevice();
      log (*lExc , "No device with number ", Integer(i, IntFmt< hex, fixed>() ));
      throw *lExc;
      return 1;
    }
    return 0;
  }

  DevAddr
  UIOPCI::decodeAddress (uint32_t uaddr) {
    DevAddr da;
    da.device = (uaddr&ADDR_DEV_MASK)>>ADDR_DEV_OFFSET;
    da.word = (uaddr&ADDR_WORD_MASK);
    return da;
  }

  ValHeader
  UIOPCI::implementWrite (const uint32_t& aAddr, const uint32_t& aValue) {
    DevAddr da = decodeAddress(aAddr);
    if (checkDevice(da.device)){ return ValWord<uint32_t>();}
    uint32_t writeval = aValue;
    
    hw[da.device][da.word] = writeval;
    return ValHeader();
  }

  ValHeader 
  UIOPCI::implementBOT(){
    log ( Debug() , "Byte Order Transaction");
    uhal::exception::UnimplementedFunctionUIOPCI* lExc = new uhal::exception::UnimplementedFunctionUIOPCI();
    log (*lExc, "Function implementBOT() is not yet implemented.");
    throw *lExc;
    return ValHeader();
  }

  ValHeader 
  UIOPCI::implementWriteBlock (const uint32_t& aAddr, const std::vector<uint32_t>& aValues, const defs::BlockReadWriteMode& aMode) {
    DevAddr da = decodeAddress(aAddr);
    if (checkDevice(da.device)) return ValWord<uint32_t>();
    uint32_t lAddr ( da.word );
    std::vector<uint32_t>::const_iterator ptr;
    for (ptr = aValues.begin(); ptr < aValues.end(); ptr++){
      uint32_t writeval = *ptr;

      hw[da.device][lAddr] = writeval;
      if ( aMode == defs::INCREMENTAL )
	lAddr ++;
    }
    return ValHeader();
  }

  ValWord<uint32_t>
  UIOPCI::implementRead (const uint32_t& aAddr, const uint32_t& aMask) {
    DevAddr da = decodeAddress(aAddr);
    if (checkDevice(da.device)) {return ValWord<uint32_t>();}
    uint32_t readval;
    readval = hw[da.device][da.word];
    ValWord<uint32_t> vw(readval, aMask);
    valwords.push_back(vw);
    primeDispatch();
    return vw;
  }
    
  ValVector< uint32_t > 
  UIOPCI::implementReadBlock ( const uint32_t& aAddr, const uint32_t& aSize, const defs::BlockReadWriteMode& aMode ) {
    DevAddr da = decodeAddress(aAddr);
    uint32_t lAddr ( da.word );
    if (checkDevice(da.device)) return ValVector<uint32_t>();
    std::vector<uint32_t> read_vector(aSize);
    std::vector<uint32_t>::iterator ptr;
    for (ptr = read_vector.begin(); ptr < read_vector.end(); ptr++){
      uint32_t readval;
      readval = hw[da.device][lAddr];
      *ptr = readval;
      if ( aMode == defs::INCREMENTAL )
	lAddr ++;
    }
    return ValVector< uint32_t> (read_vector);
  }

  void
  UIOPCI::primeDispatch () {
    // uhal will never call implementDispatch unless told that buffers are in
    // use (even though the buffers are not actually used and are length zero).
    // implementDispatch will only be called once after each checkBufferSpace.
    uint32_t sendcount = 0, replycount = 0, sendavail, replyavail;
    checkBufferSpace ( sendcount, replycount, sendavail, replyavail);
  }

  void
  UIOPCI::implementDispatch (boost::shared_ptr<Buffers> aBuffers) {
    log ( Debug(), "UIOPCI: Dispatch");
    for (unsigned int i=0; i<valwords.size(); i++)
      valwords[i].valid(true);
    valwords.clear();
  }

  ValWord<uint32_t> UIOPCI::implementRMWbits ( const uint32_t& aAddr , const uint32_t& aANDterm , const uint32_t& aORterm ){
    DevAddr da = decodeAddress(aAddr);
    if (checkDevice(da.device)) return ValWord<uint32_t>();

    //read the current value
    uint32_t readval;
    readval = hw[da.device][da.word];

    //apply and and or operations
    readval &= aANDterm;
    readval |= aORterm;
    hw[da.device][da.word] = readval;
    readval = hw[da.device][da.word];
    return ValWord<uint32_t>(readval);
  }


  ValWord< uint32_t > UIOPCI::implementRMWsum ( const uint32_t& aAddr , const int32_t& aAddend ) {
    DevAddr da = decodeAddress(aAddr);
    if (checkDevice(da.device)) return ValWord<uint32_t>();

    //read the current value
    uint32_t readval;
    readval = hw[da.device][da.word];
    //apply and and or operations
    readval += aAddend;
    hw[da.device][da.word] = readval;
    readval = hw[da.device][da.word];
    return ValWord<uint32_t>(readval);
  }
}
