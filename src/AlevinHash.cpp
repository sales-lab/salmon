#include "AlevinHash.hpp"

void getTxpToGeneMap(spp::sparse_hash_map<uint32_t, uint32_t>& txpToGeneMap,
                     std::vector<std::string>& transcripts,
                     const std::string& geneMapFile,
                     spp::sparse_hash_map<std::string, uint32_t>& geneIdxMap){
  std::ifstream t2gFile(geneMapFile);

  spp::sparse_hash_map<std::string, uint32_t> txpIdxMap(transcripts.size());

  for (size_t i=0; i<transcripts.size(); i++){
    txpIdxMap[ transcripts[i] ] = i;
  }

  uint32_t tid, gid, geneCount{0};
  std::string tStr, gStr;
  if(t2gFile.is_open()) {
    while( not t2gFile.eof() ) {
      t2gFile >> tStr >> gStr;

      if(not txpIdxMap.contains(tStr)){
        continue;
      }
      tid = txpIdxMap[tStr];

      if (geneIdxMap.contains(gStr)){
        gid = geneIdxMap[gStr];
      }
      else{
        gid = geneCount;
        geneIdxMap[gStr] = gid;
        geneCount++;
      }

      txpToGeneMap[tid] = gid;
    }
    t2gFile.close();
  }
  if(txpToGeneMap.size() < transcripts.size()){
    std::cerr << "ERROR: "
              << "Txp to Gene Map not found for "
              << transcripts.size() - txpToGeneMap.size()
              <<" transcripts. Exiting" << std::flush;
    exit(1);
  }
}

size_t readBfh(bfs::path& eqFilePath,
               std::vector<std::string>& txpNames,
               size_t bcLength,
               EqMapT &countMap,
               std::vector<std::string>& bcNames,
               CFreqMapT& freqCounter
               ) {
  std::ifstream equivFile(eqFilePath.string());

  size_t numReads{0};
  size_t numTxps, numBcs, numEqclasses;

  // Number of transcripts
  equivFile >> numTxps;

  // Number of barcodes
  equivFile >> numBcs;

  // Number of equivalence classes
  equivFile >> numEqclasses;

  txpNames.resize(numTxps);
  for (size_t i=0; i<numTxps; i++) {
    equivFile >> txpNames[i] ;
  }

  bcNames.resize(numBcs);
  for (size_t i=0; i<numBcs; i++) {
    equivFile >> bcNames[i] ;
    if (bcNames[i].size() != bcLength) {
      fmt::print(stderr, "CB {} has wrong length", bcNames[i]);
      std::cerr<<std::endl<<std::flush;;
      return 0;
    }
  }

  countMap.set_max_resize_threads(1);
  countMap.reserve(1000000);

  alevin::types::AlevinUMIKmer umiObj;
  //printing on screen progress
  const char RESET_COLOR[] = "\x1b[0m";
  char green[] = "\x1b[30m";
  green[3] = '0' + static_cast<char>(fmt::GREEN);
  char red[] = "\x1b[30m";
  red[3] = '0' + static_cast<char>(fmt::RED);
  std::cerr<<std::endl;

  for (size_t i=0; i<numEqclasses; i++) {
    uint32_t count;
    size_t labelSize ;
    equivFile >> labelSize;

    std::vector<uint32_t> txps(labelSize);
    for (auto& tid : txps) { equivFile >> tid; }
    auto txGroup = TranscriptGroup (txps);

    size_t bgroupSize;
    equivFile >> count >> bgroupSize;

    uint32_t countValidator {0};
    for (size_t j=0; j<bgroupSize; j++){
      uint32_t bc;
      size_t ugroupSize;

      equivFile >> bc >> ugroupSize;
      std::string bcName = bcNames[bc];

      for (size_t k=0; k<ugroupSize; k++){
        std::string umiSeq;
        uint64_t umiIndex;
        uint32_t umiCount;
        equivFile >> umiSeq >> umiCount;

        bool isUmiIdxOk = umiObj.fromChars(umiSeq);
        if(isUmiIdxOk){
          umiIndex = umiObj.word(0);
          auto upfn = [bc, umiIndex, umiCount](SCTGValue& x) -> void {
            // update the count
            x.count += umiCount;
            x.updateBarcodeGroup(bc, umiIndex, umiCount);
          };

          SCTGValue value(umiCount, bc, umiIndex, true);
          countMap.upsert(txGroup, upfn, value);
          freqCounter[bcName] += umiCount;
        }
        countValidator += umiCount;
      }// end-ugroup for
    }//end-bgroup for

    if (count != countValidator){
      fmt::print(stderr, "BFH eqclass count mismatch"
                 "{} Orignial, validator {} "
                 "Eqclass number {}",
                 count, countValidator, i);
      std::cerr<<std::endl<<std::flush;;
      return 0;
    }

    numReads += countValidator;
    double completionFrac = i*100.0/numEqclasses;
    uint32_t percentCompletion {static_cast<uint32_t>(completionFrac)};
    if ( percentCompletion % 10 == 0 || percentCompletion > 95) {
      fmt::print(stderr, "\r{}Done Reading : {}{}%{}",
                 green, red, percentCompletion, RESET_COLOR);
    }
  }//end-eqclass for
  std::cerr<<std::endl;
  equivFile.close();

  return numReads;
}

template <typename ProtocolT>
int salmonHashQuantify(AlevinOpts<ProtocolT>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter) {

  bool hasWhitelist = boost::filesystem::exists(aopt.whitelistFile);
  TrueBcsT trueBarcodes;

  if( hasWhitelist ){
    alevin::utils::readWhitelist(aopt.whitelistFile,
                                 trueBarcodes);
    aopt.jointLog->info("Done importing white-list Barcodes");
    aopt.jointLog->info("Total {} white-listed Barcodes", trueBarcodes.size());
  }


  EqMapT countMap;
  size_t numReads {0};
  std::vector<std::string> txpNames, bcNames;
  { // Populating Bfh
    aopt.jointLog->info("Reading BFH");
    aopt.jointLog->flush();

    numReads = readBfh( aopt.bfhFile,
                        txpNames,
                        aopt.protocol.barcodeLength,
                        countMap, bcNames,
                        freqCounter );
    if( numReads == 0 ){
      aopt.jointLog->error("can't read bfh");
      aopt.jointLog->flush();
      std::exit(74);
    }

    aopt.jointLog->info("Fount total {} reads in bfh Mode",
                        numReads);
    aopt.jointLog->flush();
  } // Done populating Bfh

  // extracting meta data for calling alevinOptimize
  aopt.jointLog->info("Reading transcript to gene Map");
  spp::sparse_hash_map<uint32_t, uint32_t> txpToGeneMap;
  spp::sparse_hash_map<std::string, uint32_t> geneIdxMap;
  getTxpToGeneMap(txpToGeneMap,
                  txpNames,
                  aopt.geneMapFile.string(),
                  geneIdxMap);

  GZipWriter gzw(outputDirectory, aopt.jointLog);
  alevinOptimize(bcNames, txpToGeneMap, geneIdxMap,
                 countMap, aopt, gzw, freqCounter, 0);
  return 0;
}


template
int salmonHashQuantify(AlevinOpts<apt::Chromium>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::ChromiumV3>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::Gemcode>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::DropSeq>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::InDrop>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::CELSeq>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::CELSeq2>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::QuartzSeq2>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
template
int salmonHashQuantify(AlevinOpts<apt::Custom>& aopt,
                       bfs::path& indexDirectory,
                       bfs::path& outputDirectory,
                       CFreqMapT& freqCounter);
