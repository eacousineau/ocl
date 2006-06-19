/***************************************************************************
 tag: Wim Meeussen and Johan Rutgeerts  Mon Jan 19 14:11:20 CET 2004   
       Ruben Smits Fri 12 08:31 CET 2006
                           -------------------
    begin                : Mon January 19 2004
    copyright            : (C) 2004 Peter Soetens
    email                : first.last@mech.kuleuven.ac.be
 
 ***************************************************************************
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Lesser General Public            *
 *   License as published by the Free Software Foundation; either          *
 *   version 2.1 of the License, or (at your option) any later version.    *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/ 

#include "Kuka361nAxesVelocityController.hpp"

#include <execution/TemplateFactories.hpp>
#include <corelib/Logger.hpp>
#include <corelib/Attribute.hpp>

namespace Orocos
{
  using namespace RTT;
  using namespace std;
  
#define NUM_AXES 6

#define KUKA361_ENCODEROFFSETS { 1000004, 1000000, 1000002, 999995, 999048, 1230656 }

#define KUKA361_CONV1  94.14706
#define KUKA361_CONV2  -103.23529
#define KUKA361_CONV3  51.44118
#define KUKA361_CONV4  175
#define KUKA361_CONV5  150
#define KUKA361_CONV6  131.64395

#define KUKA361_ENC_RES  4096

  // Conversion from encoder ticks to radiants
#define KUKA361_TICKS2RAD { 2*M_PI / (KUKA361_CONV1 * KUKA361_ENC_RES), 2*M_PI / (KUKA361_CONV2 * KUKA361_ENC_RES), 2*M_PI / (KUKA361_CONV3 * KUKA361_ENC_RES), 2*M_PI / (KUKA361_CONV4 * KUKA361_ENC_RES), 2*M_PI / (KUKA361_CONV5 * KUKA361_ENC_RES), 2*M_PI / (KUKA361_CONV6 * KUKA361_ENC_RES)}
  
  // Conversion from angular speed to voltage
#define KUKA361_RADproSEC2VOLT { 2.4621427, 2.6263797, 1.3345350, 2.3170010, 1.9720996, 1.7094233 } //23 juli 2003
  
  
  Kuka361nAxesVelocityController::Kuka361nAxesVelocityController(string name,string propertyfile)
    : GenericTaskContext(name),
      _driveValue(NUM_AXES),
      _positionValue(NUM_AXES),
      _propertyfile(propertyfile),
      _driveLimits("driveLimits","velocity limits of the axes, (rad/s)"),
      _lowerPositionLimits("LowerPositionLimits","Lower position limits (rad)"),
      _upperPositionLimits("UpperPositionLimits","Upper position limits (rad)"),
      _initialPosition("initialPosition","Initial position (rad) for simulation or hardware"),
      _driveOffset("driveOffset","offset (in rad/s) to the drive value."),
      _simulation("simulation","true if simulationAxes should be used"),
      _activated(false),
      _positionConvertFactor(NUM_AXES),
      _driveConvertFactor(NUM_AXES),
#if (defined OROPKG_OS_LXRT && defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
      _axes_hardware(NUM_AXES),
#endif
      _axes(NUM_AXES),
      _axes_simulation(NUM_AXES)
  {
    double ticks2rad[NUM_AXES] = KUKA361_TICKS2RAD;
    double vel2volt[NUM_AXES] = KUKA361_RADproSEC2VOLT;
    int encoderOffsets[NUM_AXES] = KUKA361_ENCODEROFFSETS;
    for(unsigned int i = 0;i<NUM_AXES;i++){
      _positionConvertFactor[i] = ticks2rad[i];
      _driveConvertFactor[i] = vel2volt[i];
    }
    
    attributes()->addProperty( &_driveLimits );
    attributes()->addProperty( &_lowerPositionLimits );
    attributes()->addProperty( &_upperPositionLimits  );
    attributes()->addProperty( &_initialPosition  );
    attributes()->addProperty( &_driveOffset  );
    attributes()->addProperty( &_simulation  );
    attributes()->addConstant( "positionConvertFactor",_positionConvertFactor  );
    attributes()->addConstant( "driveConvertFactor",_driveConvertFactor  );
    attributes()->addConstant( "NUM_AXES", NUM_AXES);
    
    if (!readProperties(_propertyfile)) {
      Logger::log() << Logger::Error << "Failed to read the property file, continueing with default values." << Logger::endl;
    }  
    
#if (defined OROPKG_OS_LXRT && defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    _comediDev        = new ComediDevice( 1 );
    _comediSubdevAOut = new ComediSubDeviceAOut( _comediDev, "Kuka361" );
    _apci1710         = new EncoderSSI_apci1710_board( 0, 1 );
    _apci2200         = new RelayCardapci2200( "Kuka361" );
    _apci1032         = new SwitchDigitalInapci1032( "Kuka361" );
        
        
    for (unsigned int i = 0; i < NUM_AXES; i++){
      //Setting up encoders
      _encoderInterface[i] = new EncoderSSI_apci1710( i + 1, _apci1710 );
      _encoder[i]          = new AbsoluteEncoderSensor( _encoderInterface[i], 1.0 / ticks2rad[i], encoderOffsets[i], -10, 10 );
      
      _brake[i] = new DigitalOutput( _apci2200, i + KUKA361_NUM_AXIS );
      _brake[i]->switchOn();
      
      _vref[i]   = new AnalogOutput<unsigned int>( _comediSubdevAOut, i );
      _enable[i] = new DigitalOutput( _apci2200, i );
      _drive[i] = new AnalogDrive( _vref[i], _enable[i], 1.0 / vel2volt[i], _driveOffsets.value()[i]);
      
      _axes_hardware[i] = new ORO_DeviceDriver::Axis( _drive[i] );
      _axes_hardware[i]->limitDrive( _driveLimits.value()[i] );
      //_axes[i]->setLimitDriveEvent( maximumDrive );
      _axes_hardware[i]->setBrake( _brake[i] );
      _axes_hardware[i]->setSensor( "Position", _encoder[i] );
    }
    
#endif
    for (unsigned int i = 0; i <NUM_AXES; i++)
      {
  	_axes_simulation[i] = new ORO_DeviceDriver::SimulationAxis(_initialPosition.value()[i],_lowerPositionLimits.value()[i],_upperPositionLimits.value()[i]);
  	_axes_simulation[i]->setMaxDriveValue( _driveLimits.value()[i] );
      }

#if (defined OROPKG_OS_LXRT && defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.value()){
      for (unsigned int i = 0; i <NUM_AXES; i++)
	_axes[i] = _axes_hardware[i];
      Logger::log() << Logger::Info << "LXRT version of LiASnAxesVelocityController has started" << Logger::endl;
    }
    else{
      for (unsigned int i = 0; i <NUM_AXES; i++)
	_axes[i] = _axes_simulation[i];
      Logger::log() << Logger::Info << "LXRT simulation version of Kuka361nAxesVelocityController has started" << Logger::endl;
    }
#else
    for (unsigned int i = 0; i <NUM_AXES; i++)
      _axes[i] = _axes_simulation[i];
    Logger::log() << Logger::Info << "GNULINUX simulation version of Kuka361nAxesVelocityController has started" << Logger::endl;
#endif
    
    // make task context
    /*
     *  Command Interface
     */
    typedef Kuka361nAxesVelocityController MyType;
    TemplateCommandFactory<MyType>* cfact = newCommandFactory( this );
    cfact->add( "startAxis",         command( &MyType::startAxis,         &MyType::startAxisCompleted, "start axis, initializes drive value to zero and starts updating the drive-value with the drive-port (only possible if axis is unlocked)","axis","axis to start" ) );
    cfact->add( "stopAxis",          command( &MyType::stopAxis,          &MyType::stopAxisCompleted, "stop axis, sets drive value to zero and disables the update of the drive-port, (only possible if axis is started)","axis","axis to stop" ) );
    cfact->add( "lockAxis",          command( &MyType::lockAxis,          &MyType::lockAxisCompleted, "lock axis, enables the brakes (only possible if axis is stopped)","axis","axis to lock" ) );
    cfact->add( "unlockAxis",        command( &MyType::unlockAxis,        &MyType::unlockAxisCompleted, "unlock axis, disables the brakes and enables the drive (only possible if axis is locked)","axis","axis to unlock" ) );
    cfact->add( "startAllAxes",      command( &MyType::startAllAxes,      &MyType::startAllAxesCompleted, "start all axes" ) );
    cfact->add( "stopAllAxes",       command( &MyType::stopAllAxes,       &MyType::stopAllAxesCompleted, "stops all axes" ) );
    cfact->add( "lockAllAxes",       command( &MyType::lockAllAxes,       &MyType::lockAllAxesCompleted, "locks all axes" ) );
    cfact->add( "unlockAllAxes",     command( &MyType::unlockAllAxes,     &MyType::unlockAllAxesCompleted, "unlock all axes" ) );
    cfact->add( "prepareForUse",     command( &MyType::prepareForUse,     &MyType::prepareForUseCompleted, "prepares the robot for use" ) );
    cfact->add( "prepareForShutdown",command( &MyType::prepareForShutdown,&MyType::prepareForShutdownCompleted, "prepares the robot for shutdown" ) );
    cfact->add( "addDriveOffset"    ,command( &MyType::addDriveOffset,    &MyType::addDriveOffsetCompleted,  "adds an offset to the drive value of axis","axis","axis to add offset to","offset","offset value in rad/s") );
    this->commands()->registerObject("this", cfact);
    
    /**
     * Creating and adding the data-ports
     */
    for (int i=0;i<NUM_AXES;++i) {
        char buf[80];
        sprintf(buf,"driveValue%d",i);
        _driveValue[i] = new ReadDataPort<double>(buf);
        ports()->addPort(_driveValue[i]);
        sprintf(buf,"positionValue%d",i);
        _positionValue[i]  = new WriteDataPort<double>(buf);
        ports()->addPort(_positionValue[i]);
    }
    
    /**
     * Adding the events :
     */
    events()->addEvent( "driveOutOfRange", &_driveOutOfRange );
    events()->addEvent( "positionOutOfRange", &_positionOutOfRange );
    
    /**
     * Connecting EventC to Event make c++-emit possible
     */
    _driveOutOfRange_event = events()->setupEmit("driveOutOfRange").arg(_driveOutOfRange_axis).arg(_driveOutOfRange_value);
    _positionOutOfRange_event = events()->setupEmit("positionOutOfRange").arg(_positionOutOfRange_axis).arg(_positionOutOfRange_value);
  }
  
  Kuka361nAxesVelocityController::~Kuka361nAxesVelocityController()
  {
    // make sure robot is shut down
    prepareForShutdown();
    
    // brake, drive, sensors and switches are deleted by each axis
    for (unsigned int i = 0; i < NUM_AXES; i++)
      delete _axes_simulation[i];
    
#if (defined OROPKG_OS_LXRT && defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    for (unsigned int i = 0; i < NUM_AXES; i++)
      delete _axes_hardware[i];
    delete _comediDev;
    delete _comediSubdevAOut;
    delete _apci1710;
    delete _apci2200;
    delete _apci1032;
#endif
  }
  
  
  bool Kuka361nAxesVelocityController::startup()
  {
    return true;
  }
  
  void Kuka361nAxesVelocityController::update()
  {
    for (int axis=0;axis<NUM_AXES;axis++) {      
      // Ask the position and perform checks in joint space.
      _positionValue[axis]->Set(_axes[axis]->getSensor("Position")->readSensor());
      
      if((_positionValue[axis]->Get() < _lowerPositionLimits.value()[axis]) 
         ||(_positionValue[axis]->Get() > _upperPositionLimits.value()[axis])
         ) {
        _positionOutOfRange_axis = axis;
	_positionOutOfRange_value = _positionValue[axis]->Get();
	_positionOutOfRange_event.emit();
      }
      
      // send the drive value to hw and performs checks
      if (_axes[axis]->isDriven()) {
        if ((_driveValue[axis]->Get() < -_driveLimits.value()[axis]) 
  	  || (_driveValue[axis]->Get() >  _driveLimits.value()[axis]))
  	{
	  _driveOutOfRange_axis = axis;
	  _driveOutOfRange_value = _driveValue[axis]->Get();
	  _driveOutOfRange_event.emit();
  	}
        else{
  	_axes[axis]->drive(_driveValue[axis]->Get());
        }
      }
    }
  }
  
  
  void Kuka361nAxesVelocityController::shutdown()
  {
    //Make sure machine is shut down
    prepareForShutdown();
    //Write properties back to file
#if (defined OROPKG_OS_LXRT&& defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.value())
      for(unsigned int i = 0;i<NUM_AXES;i++)    
	_driveOffset.set()[i] = ((Axis*)_axes[i])->getDrive()->getOffset();  
#endif
    writeProperties(_propertyfile);
  }
  
  
  bool Kuka361nAxesVelocityController::prepareForUse()
  {
#if (defined OROPKG_OS_LXRT&& defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.value()){
      _apci2200->switchOn( 12 );
      _apci2200->switchOn( 14 );
      Logger::log()<<Logger::Warning<<"Release Emergency stop and push button to start ...."<<Logger::endl;
    }
#endif
    _activated = true;
    return true;
  }
  
  bool Kuka361nAxesVelocityController::prepareForUseCompleted()const
  {
#if (defined OROPKG_OS_LXRT&& defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.rvalue())
      if (_apci1032->isOn(12) && _apci1032->isOn(14));
#endif
    return true;
  }
  
  bool Kuka361nAxesVelocityController::prepareForShutdown()
  {
    //make sure all axes are stopped and locked
    stopAllAxes();
    lockAllAxes();
#if (defined OROPKG_OS_LXRT&& defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.value()){
      _apci2200->switchOff( 12 );
      _apci2200->switchOff( 14 );
    }
    
#endif
    _activated = false;
    return true;
  }
  
  bool Kuka361nAxesVelocityController::prepareForShutdownCompleted()const
  {
    return true;
  }
  
  bool Kuka361nAxesVelocityController::stopAxisCompleted(int axis)const
  {
    return _axes[axis]->isStopped();
  }
  
  bool Kuka361nAxesVelocityController::lockAxisCompleted(int axis)const
  {
    return _axes[axis]->isLocked();
  }
  
  bool Kuka361nAxesVelocityController::startAxisCompleted(int axis)const
  {
    return _axes[axis]->isDriven();
  }
  
  bool Kuka361nAxesVelocityController::unlockAxisCompleted(int axis)const
  {
    return !_axes[axis]->isLocked();
  }
  
  bool Kuka361nAxesVelocityController::stopAxis(int axis)
  {
    if (!(axis<0 || axis>NUM_AXES-1))
      return _axes[axis]->stop();
    else{
      Logger::log()<<Logger::Error<<"Axis "<< axis <<"doesn't exist!!"<<Logger::endl;
      return false;
    }
  }
  
  bool Kuka361nAxesVelocityController::startAxis(int axis)
  {
    if (!(axis<0 || axis>NUM_AXES-1))
      return _axes[axis]->drive(0.0);
    else{
      Logger::log()<<Logger::Error<<"Axis "<< axis <<"doesn't exist!!"<<Logger::endl;
      return false;
    }
  }
  
  bool Kuka361nAxesVelocityController::unlockAxis(int axis)
  {
    if(_activated){
      if (!(axis<0 || axis>NUM_AXES-1))
        return _axes[axis]->unlock();
      else{
        Logger::log()<<Logger::Error<<"Axis "<< axis <<"doesn't exist!!"<<Logger::endl;
        return false;
      }
    }
    else
      return false;
  }
  
  bool Kuka361nAxesVelocityController::lockAxis(int axis)
  {
    if (!(axis<0 || axis>NUM_AXES-1))
      return _axes[axis]->lock();
    else{
      Logger::log()<<Logger::Error<<"Axis "<< axis <<"doesn't exist!!"<<Logger::endl;
      return false;
    }
  }
  
  bool Kuka361nAxesVelocityController::stopAllAxes()
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++){
      _return &= stopAxis(i);
    }
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::startAllAxes()
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++){
      _return &= startAxis(i);
    }
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::unlockAllAxes()
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++){
      _return &= unlockAxis(i);
      }
      return _return;
  }
  
  bool Kuka361nAxesVelocityController::lockAllAxes()
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++){
      _return &= lockAxis(i);
    }
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::stopAllAxesCompleted()const
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++)
      _return &= stopAxisCompleted(i);
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::startAllAxesCompleted()const
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++)
     _return &= startAxisCompleted(i);
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::lockAllAxesCompleted()const
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++)
      _return &= lockAxisCompleted(i);
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::unlockAllAxesCompleted()const
  {
    bool _return = true;
    for(unsigned int i = 0;i<NUM_AXES;i++)
      _return &= unlockAxisCompleted(i);
    return _return;
  }
  
  bool Kuka361nAxesVelocityController::addDriveOffset(int axis, double offset)
  {
#if (defined OROPKG_OS_LXRT&& defined OROPKG_DEVICE_DRIVERS_COMEDI&& defined (OROPKG_DEVICE_DRIVERS_APCI))
    if(!_simulation.value())
      ((Axis*)_axes[axis])->getDrive()->addOffset(offset);  
#endif
    return true;
  }
  
  bool Kuka361nAxesVelocityController::addDriveOffsetCompleted(int axis)const
  {
    return true;
  }
  
}//namespace orocos