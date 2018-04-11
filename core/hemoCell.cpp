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
#include "hemocell.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

//Required as extern from constant_defaults
int verbose = 0;

volatile sig_atomic_t interrupted = 0;
void set_interrupt(int signum) {
  interrupted = 1;
}

HemoCell::HemoCell(char * configFileName, int argc, char * argv[]) {
  plbInit(&(argc),&(argv));
  printHeader();

	//TODO This should be done through hemocell config, not some palabos global
  global::directories().setOutputDir("./tmp/");
  global::directories().setInputDir("./");
  global::IOpolicy().activateParallelIO(true);
  global::IOpolicy().setStlFilesHaveLowerBound(false);
    hlog << "(HemoCell) (Config) reading " << configFileName << endl;
  cfg = new Config(configFileName);
  documentXML = new XMLreader(configFileName);
  try {
    verbose = (*cfg)["verbose"].read<int>();
  } catch (std::invalid_argument & exeption) {}

  //setting outdir
  try {
    std::string outDir = (*cfg)["parameters"]["outputDirectory"].read<string>() + "/";
    if (outDir[0] != '/') {
          outDir = "./" + outDir;
    }
    global::directories().setOutputDir(outDir);
  } catch (std::invalid_argument & exeption) {}
  mkpath((global::directories().getOutputDir() + "/hdf5/").c_str(), 0777);

  //Setting logfile and logdir  
  global::directories().setLogOutDir("./log/");
  try {
    std::string outDir = (*cfg)["parameters"]["logDirectory"].read<string>() + "/";
    if (outDir[0] != '/') {
          outDir = "./" + outDir;
    }
    global::directories().setLogOutDir(outDir);
  } catch (std::invalid_argument & exeption) {}
  string logfilename = "logfile";
  try {
    logfilename = (*cfg)["parameters"]["logFile"].read<string>();
  } catch (std::invalid_argument & exeption) {}
  mkpath(global::directories().getLogOutDir().c_str(), 0777);

  if (global::mpi().getRank() == 0) {
    string filename =  global::directories().getLogOutDir() + logfilename;
    if (!file_exists(filename)) { 
      hlog.logfile.open(global::directories().getLogOutDir() + logfilename , std::fstream::out );
      goto logfile_open_done; 
    }
    for (int i = 0; i < INT_MAX; i++) {
      if (!file_exists(filename + "." + to_string(i))) {
        hlog.logfile.open(filename + "." + to_string(i) , std::fstream::out);
        goto logfile_open_done; 
      }
    }
    logfile_open_done:
    if (!hlog.logfile.good()) {
      pcerr << "(HemoCell) (LogFile) Error opening logfile, exiting" << endl;
      exit(1);
    }
  }
  
  // start clock for basic performance feedback
  lastOutputAt = 0;
  global::timer("atOutput").start();
  
#ifdef FORCE_LIMIT
    hlogfile << "(HemoCell) WARNING: Force limit active at " << FORCE_LIMIT << " pN. Results can be inaccurate due to force capping." << endl;
#endif
  if (sizeof(T) == sizeof(float)) {
    hlog << "(HemoCell) WARNING: Running with single precision, you might want to switch to double precision" << endl;
  }
  
  ///Set signal handlers to exit gracefully on many signals
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  sa.sa_handler = set_interrupt;
  sigaction(SIGINT,&sa,0);
  sigaction(SIGTERM,&sa,0);
  sigaction(SIGHUP,&sa,0);
  sigaction(SIGQUIT,&sa,0);
  sigaction(SIGABRT,&sa,0);
  sigaction(SIGUSR1,&sa,0);
  sigaction(SIGUSR2,&sa,0);
 
  
}

void HemoCell::latticeEquilibrium(T rho, hemo::Array<T, 3> vel) {
  hlog << "(HemoCell) (Fluid) Setting Fluid Equilibrium" << endl;
  plb::Array<T,3> vel_plb = {vel[0],vel[1],vel[2]};
  initializeAtEquilibrium(*lattice, (*lattice).getBoundingBox(), rho, vel_plb);
}

void HemoCell::initializeCellfield() {
  cellfields = new HemoCellFields(*lattice,(*cfg)["domain"]["particleEnvelope"].read<int>(),*this);

  //Correct place for init
  loadBalancer = new LoadBalancer(*this);
}

void HemoCell::setOutputs(string name, vector<int> outputs) {
  hlog << "(HemoCell) (CellField) Setting output variables for " << name << " cells" << endl;
  vector<int> outputs_c = outputs;
  (*cellfields)[name]->setOutputVariables(outputs_c);
}

void HemoCell::setFluidOutputs(vector<int> outputs) {
  hlog << "(HemoCell) (Fluid) Setting output variables for fluid field" << endl;
  vector<int> outputs_c = outputs;
  cellfields->desiredFluidOutputVariables = outputs_c;
}

void HemoCell::setSystemPeriodicity(unsigned int axis, bool bePeriodic) {
  if (lattice == 0) {
    pcerr << "(HemoCell) (Periodicity) please create a lattice before trying to set the periodicity" << endl;
    exit(0);    
  }
  if (cellfields->immersedParticles == 0) {
    pcerr << "(HemoCell) (Periodicity) please create a particlefield (hemocell.initializeCellfields()) before trying to set the periodicity" << endl;
    exit(0);   
  }
  lattice->periodicity().toggle(axis,bePeriodic);
  cellfields->immersedParticles->periodicity().toggle(axis, bePeriodic);
  cellfields->InitAfterLoadCheckpoint();
}

void HemoCell::setSystemPeriodicityLimit(unsigned int axis, int limit) {
  hlog << "(HemoCell) (Periodicity) Setting periodicity limit of axis " << axis << " to " << limit << endl;
  cellfields->periodicity_limit[axis] = limit;
  
  //recalculate offsets :
  cellfields->periodicity_limit_offset_y = cellfields->periodicity_limit[0];
  cellfields->periodicity_limit_offset_z = cellfields->periodicity_limit[0]*cellfields->periodicity_limit[1];
}

void HemoCell::loadParticles() {
  hlog << "(HemoCell) (CellField) Loading particle positions "  << endl;
  loadParticlesIsCalled = true;
  readPositionsBloodCellField3D(*cellfields, param::dx, *cfg);
  cellfields->syncEnvelopes();
  cellfields->deleteIncompleteCells(false);
}

void HemoCell::loadCheckPoint() {
  pcout << "(HemoCell) (Saving Functions) Loading Checkpoint"  << endl;
  cellfields->load(documentXML, iter, cfg);
}

void HemoCell::saveCheckPoint() {
  pcout << "(HemoCell) (Saving Functions) Saving Checkpoint at timestep " << iter << endl;
  cellfields->save(documentXML, iter, cfg);
}

void HemoCell::writeOutput() {
  // Very naive performance approximation
  T dtSinceLastOutput = global::timer("atOutput").stop();
  T timePerIter = dtSinceLastOutput / (iter - lastOutputAt);
  lastOutputAt = iter;

	pcout << "(HemoCell) (Output) writing output at timestep " << iter << " (" << param::dt * iter<< " s). Approx. performance: " << timePerIter << " s / iteration." << endl;
  if(repulsionEnabled) {
    cellfields->applyRepulsionForce();
  }
  if(boundaryRepulsionEnabled) {
    cellfields->applyBoundaryRepulsionForce();
  }
  cellfields->syncEnvelopes();
  cellfields->deleteIncompleteCells(false);

  //Repoint surfaceparticle forces for output
  cellfields->separate_force_vectors();

  //Recalculate the forces
  cellfields->applyConstitutiveModel(true);

  // Creating a new directory per save
  if (global::mpi().isMainProcessor()) {
    string folder = global::directories().getOutputDir() + "/hdf5/" + zeroPadNumber(iter) ;
    mkpath(folder.c_str(), 0777);
    folder = global::directories().getOutputDir() + "/csv/" + zeroPadNumber(iter) ;
    mkpath(folder.c_str(), 0777);
  }
  global::mpi().barrier();

  //Write Output
  writeCellField3D_HDF5(*cellfields,param::dx,param::dt,iter);
  writeFluidField_HDF5(*cellfields,param::dx,param::dt,iter);
  writeCellInfo_CSV(this);

  //Repoint surfaceparticle forces for speed
  cellfields->unify_force_vectors();

  // Continue with performance measurement
  global::timer("atOutput").restart();
}

void HemoCell::checkExitSignals() {
  if (interrupted == 1) {
    cout << endl << "Caught Signal, saving work and quitting!" << endl;
    exit(1);
  }
}

void HemoCell::iterate() {
  checkExitSignals();

  // ### 1 ### Particle Force to Fluid
  if(repulsionEnabled && iter % cellfields->repulsionTimescale == 0) {
    cellfields->applyRepulsionForce();
  }
  if(boundaryRepulsionEnabled && iter % cellfields->boundaryRepulsionTimescale == 0) {
    cellfields->applyBoundaryRepulsionForce();
  }
  cellfields->spreadParticleForce();
  
  // #### 2 #### LBM
  lattice->timedCollideAndStream();

  if(iter %cellfields->particleVelocityUpdateTimescale == 0) {
    // #### 3 #### IBM interpolation
    cellfields->interpolateFluidVelocity();

    // ### 4 ### sync the particles
    cellfields->syncEnvelopes();
  }
  
  // ### 5 ###
  cellfields->advanceParticles();

  // ### 6 ###
  cellfields->applyConstitutiveModel();    // Calculate Force on Vertices
  
  //We can safely delete non-local cells here, assuming model timestep is divisible by velocity timestep
  if(iter % cellfields->particleVelocityUpdateTimescale == 0) {
    cellfields->deleteNonLocalParticles(3);
  }

  // Reset Forces on the lattice, TODO do own efficient implementation
  setExternalVector(*lattice, (*lattice).getBoundingBox(),
          DESCRIPTOR<T>::ExternalField::forceBeginsAt,
          plb::Array<T, DESCRIPTOR<T>::d>(0.0, 0.0, 0.0));

  iter++;
}

T HemoCell::calculateFractionalLoadImbalance() {
  hlog << "(HemoCell) (LoadBalancer) Calculating Fractional Load Imbalance at timestep " << iter << endl;
  return loadBalancer->calculateFractionalLoadImbalance();
}

void HemoCell::setMaterialTimeScaleSeparation(string name, unsigned int separation){
  hlog << "(HemoCell) (Timescale Seperation) Setting seperation of " << name << " to " << separation << " timesteps"<<endl;
  //pcout << "(HemoCell) WARNING if the timescale separation is not dividable by tmeasure, checkpointing is non-deterministic!"<<endl; //not true anymore, with checkpointing remaining force is saved
  (*cellfields)[name]->timescale = separation;
  if (separation%cellfields->particleVelocityUpdateTimescale!=0) {
     pcout << "(HemoCell) Error, Velocity timescale separation cannot divide this material timescale separation, exiting ..." <<endl;
     exit(0);
  }
}

void HemoCell::setParticleVelocityUpdateTimeScaleSeparation(unsigned int separation) {
  hlog << "(HemoCell) (Timescale separation) Setting update separation of all particles to " << separation << " timesteps" << endl;
  if(verbose >= 2) {
    hlog << "(HemoCell) WARNING this introduces great errors" << endl;
  }
  for (unsigned int i = 0; i < cellfields->size() ; i++) {
    if ((*cellfields)[i]->timescale%separation !=0) {
      pcout << "(HemoCell) Error, Velocity timescale separation cannot divide all material timescale separations, exiting ..." <<endl;
      exit(0);
    }
  }
  cellfields->particleVelocityUpdateTimescale = separation;
}

void HemoCell::setRepulsionTimeScaleSeperation(unsigned int separation){
  hlog << "(HemoCell) (Repulsion Timescale Seperation) Setting seperation to " << separation << " timesteps"<<endl;
  cellfields->repulsionTimescale = separation;
  if (separation%cellfields->particleVelocityUpdateTimescale!=0) {
     pcout << "(HemoCell) Error, Velocity timescale separation cannot divide this repulsion timescale separation, exiting ..." <<endl;
     exit(0);
  }
}

void HemoCell::setMinimumDistanceFromSolid(string name, T distance) {
  hlog << "(HemoCell) (Set Distance) Setting minimum distance from solid to " << distance << " micrometer for " << name << endl; 
  if (loadParticlesIsCalled) {
    pcout << "(HemoCell) (Set Distance) WARNING: this function is called after the particles are loaded, so it probably has no effect" << endl;
  }
  (*cellfields)[name]->minimumDistanceFromSolid = distance;
}

void HemoCell::setRepulsion(T repulsionConstant, T repulsionCutoff) {
  hlog << "(HemoCell) (Repulsion) Setting repulsion constant to " << repulsionConstant << ". repulsionCutoff to" << repulsionCutoff << " µm" << endl;
  if(verbose >= 2) {
    pcout << "(HemoCell) (Repulsion) Enabling repulsion" << endl;
  }
  cellfields->repulsionConstant = repulsionConstant;
  cellfields->repulsionCutoff = repulsionCutoff*(1e-6/param::dx);
  repulsionEnabled = true;
}

void HemoCell::enableBoundaryParticles(T boundaryRepulsionConstant, T boundaryRepulsionCutoff, unsigned int timestep) {
  cellfields->populateBoundaryParticles();
  hlog << "(HemoCell) (Repulsion) Setting boundary repulsion constant to " << boundaryRepulsionConstant << ". boundary repulsionCutoff to" << boundaryRepulsionCutoff << " µm" << endl;
  if(verbose >= 2) {
    pcout << "(HemoCell) (Repulsion) Enabling boundary repulsion" << endl;
  }
  if (timestep%cellfields->particleVelocityUpdateTimescale!=0) {
     pcout << "(HemoCell) Error, Velocity timescale separation cannot divide this repulsion timescale separation, exiting ..." <<endl;
     exit(0);
  }
  cellfields->boundaryRepulsionConstant = boundaryRepulsionConstant;
  cellfields->boundaryRepulsionCutoff = boundaryRepulsionCutoff*(1e-6/param::dx);
  cellfields->boundaryRepulsionTimescale = timestep;
  boundaryRepulsionEnabled = true;
}



#ifdef HEMO_PARMETIS
void HemoCell::doLoadBalance() {
	pcout << "(HemoCell) (LoadBalancer) Balancing Atomic Block over mpi processes" << endl;
  loadBalancer->doLoadBalance();
}
#endif

void HemoCell::doRestructure(bool checkpoint_avail) {
  hlog << "(HemoCell) (LoadBalancer) Restructuring Atomic Blocks on processors" << endl;
  loadBalancer->restructureBlocks(checkpoint_avail);
}
