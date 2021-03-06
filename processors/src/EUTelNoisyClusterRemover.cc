/*
 *   This source code is part of the Eutelescope package of Marlin.
 *   You are free to use this source files for your own development as
 *   long as it stays in a public research context. You are not
 *   allowed to use it for commercial purpose. You must put this
 *   header with author names in all development based on this file.
 *
 */

// eutelescope includes ".h"
#include "EUTelNoisyClusterRemover.h"
#include "EUTELESCOPE.h"
#include "EUTelUtility.h"
#include "EUTelTrackerDataInterfacerImpl.h"

// marlin includes ".h"
#include "EUTelRunHeaderImpl.h"
#include "marlin/Processor.h"

// lcio includes <.h>
#include <IO/LCWriter.h>
#include <LCIOTypes.h>

#include <IMPL/LCCollectionVec.h>
#include <IMPL/TrackerPulseImpl.h>
#include <UTIL/CellIDDecoder.h>
#include <UTIL/CellIDEncoder.h>

#include <EVENT/LCCollection.h>
#include <EVENT/LCEvent.h>

// system includes <>
#include <algorithm>
#include <memory>

namespace eutelescope {

  bool EUTelNoisyClusterRemover::_staticPrintedSummary = false;

  EUTelNoisyClusterRemover::EUTelNoisyClusterRemover()
      : Processor("EUTelNoisyClusterRemover"),
        _inputCollectionName(""), _outputCollectionName(""), _iRun(0),
        _iEvt(0) {

    _description = "EUTelNoisyClusterRemover removes masked noisy "
                   "clusters (TrackerPulses) from a collection. It does "
                   "not read in a hot pixel collection, but removes masked "
                   "TrackerPulses.";

    registerInputCollection(LCIO::TRACKERPULSE, 
			    "InputCollectionName",
			    "Input collection containing noise masked tracker pulse objects",
			    _inputCollectionName,
			    std::string("noisy_cluster"));

    registerOutputCollection(LCIO::TRACKERPULSE, 
			     "OutputCollectionName",
			     "Output collection where noisy clusters have been removed",
			     _outputCollectionName,
			     std::string("noisefree_clusters"));
  }

  void EUTelNoisyClusterRemover::init() {
  
    //usually a good idea to do
    printParameters();
    
    //reset run and event counters
    _iRun = 0;
    _iEvt = 0;
  }

  void EUTelNoisyClusterRemover::processRunHeader(LCRunHeader *rdr) {
  
    std::unique_ptr<EUTelRunHeaderImpl> runHeader(new EUTelRunHeaderImpl(rdr));
    runHeader->addProcessor(type());
    //increment run counter
    ++_iRun;
    //reset event counter
    _iEvt = 0;
  }

  void EUTelNoisyClusterRemover::processEvent(LCEvent *event) {
  
    //get the collection of interest from the event
    LCCollectionVec *pulseInputCollectionVec = nullptr;
    try {
      pulseInputCollectionVec = dynamic_cast<LCCollectionVec *>(
          event->getCollection(_inputCollectionName));
    } catch(lcio::DataNotAvailableException &e) {
      return;
    }

    //prepare decoder for input data
    CellIDDecoder<TrackerPulseImpl> cellDecoder(pulseInputCollectionVec);

    //now prepare output collection
    LCCollectionVec *outputCollection = nullptr;
    bool outputCollectionExists = false;
    _initialOutputCollectionSize = -1;

    try {
      outputCollection = dynamic_cast<LCCollectionVec *>(
      event->getCollection(_outputCollectionName));
      outputCollectionExists = true;
      _initialOutputCollectionSize = outputCollection->size();
    } catch(lcio::DataNotAvailableException &e) {
      outputCollection = new LCCollectionVec(LCIO::TRACKERPULSE);
    }

    Utility::copyLCCollectionParameters(outputCollection, pulseInputCollectionVec);

    //read the encoding string from input collection
    std::string encodingString = pulseInputCollectionVec->
      getParameters().getStringVal(LCIO::CellIDEncoding);
    //and the encoder for the output data
    CellIDEncoder<TrackerPulseImpl> idZSGenDataEncoder(encodingString,
                                                       outputCollection);

    //[START] loop over all pulses
    for (size_t iPulse = 0; iPulse < pulseInputCollectionVec->size(); iPulse++) {
         
      //get the tracker pulse
      TrackerPulseImpl *inputPulse = dynamic_cast<TrackerPulseImpl *>(
		      pulseInputCollectionVec->getElementAt(iPulse));
      //and its quality
      int quality = cellDecoder(inputPulse)["quality"];
      int sensorID = cellDecoder(inputPulse)["sensorID"];

      //if kNoisyCluster flag is NOT set, add pulse to output collection
      if(!(quality & kNoisyCluster)) {
        //TrackerPulseImpl for output collection
        std::unique_ptr<TrackerPulseImpl> outputPulse =
	  std::make_unique<TrackerPulseImpl>();

        //copy information which is the same
        outputPulse->setCellID0(inputPulse->getCellID0());
        outputPulse->setCellID1(inputPulse->getCellID1());
        outputPulse->setTime(inputPulse->getTime());
        outputPulse->setCharge(inputPulse->getCharge());
        outputPulse->setCovMatrix(inputPulse->getCovMatrix());
        outputPulse->setQuality(inputPulse->getQuality());
        outputPulse->setTrackerData(inputPulse->getTrackerData());

        outputCollection->push_back(outputPulse.release());
        streamlog_out(DEBUG4) << "elements in collection : " 
			      << outputCollection->size() << std::endl;
      } else {
        //if the cluster is noisy, thus removed, count for output
        _removedNoisyPulses[sensorID]++;
      }
    }//[END] loop over all pulses

    //add the collection if necessary
    if(!outputCollectionExists &&
        (outputCollection->size() != _initialOutputCollectionSize)) {
      event->addCollection(outputCollection, _outputCollectionName);
      streamlog_out(DEBUG4) << "adding output collection: " 
			    << _outputCollectionName << " "
			    << outputCollection->size() << std::endl;
    }

    if(!outputCollectionExists &&
        (outputCollection->size() == _initialOutputCollectionSize)) {
      delete outputCollection;
    }
    //rest of memory cleaned up by smart ptrs
  }

  void EUTelNoisyClusterRemover::end() {
  
    //print out some stats for the user
    if(!_staticPrintedSummary) {
      streamlog_out(MESSAGE4) << "Noisy cluster remover(s) successfully finished" 
			      << std::endl;
      streamlog_out(MESSAGE4) << "Printing summary:" << std::endl;
      _staticPrintedSummary = true;
    }
    if(_removedNoisyPulses.empty()) {
      streamlog_out(MESSAGE4) << "No noisy pulses removed as none were found"
                              << std::endl;
    }
    for(std::map<int, int>::iterator it = _removedNoisyPulses.begin();
         it != _removedNoisyPulses.end(); ++it) {
      streamlog_out(MESSAGE4) << "Removed " << (*it).second
                              << " noisy pulses from plane " << (*it).first
                              << "." << std::endl;
    }
  }

}
