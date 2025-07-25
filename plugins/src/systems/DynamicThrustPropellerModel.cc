/*
 *
 * https://www.electricrcaircraftguy.com/2013/09/propeller-static-dynamic-thrust-equation.html
 *
 */

#include "DynamicThrustPropellerModel.hh"

#include <mutex>
#include <string>

#include <gz/msgs/actuators.pb.h>

#include <gz/common/Profiler.hh>

#include <gz/plugin/Register.hh>
#include <gz/transport/Node.hh>

#include <gz/math/Helpers.hh>
#include <gz/math/Pose3.hh>
#include <gz/math/Vector3.hh>
#include <gz/msgs/Utility.hh>

#include <sdf/sdf.hh>

#include "gz/sim/components/Actuators.hh"
#include "gz/sim/components/ExternalWorldWrenchCmd.hh"
#include "gz/sim/components/JointAxis.hh"
#include "gz/sim/components/JointVelocity.hh"
#include "gz/sim/components/JointVelocityCmd.hh"
#include "gz/sim/components/LinearVelocity.hh"
#include "gz/sim/components/ParentLinkName.hh"
#include "gz/sim/components/Pose.hh"
#include "gz/sim/components/Wind.hh"
#include "gz/sim/Link.hh"
#include "gz/sim/Model.hh"
#include "gz/sim/Util.hh"

// from rotors_gazebo_plugins/include/rotors_gazebo_plugins/common.h
/// \brief    This class can be used to apply a first order filter on a signal.
///           It allows different acceleration and deceleration time constants.
/// \details
///           Short reveiw of discrete time implementation of first order system
///           Laplace:
///             X(s)/U(s) = 1/(tau*s + 1)
///           continous time system:
///             dx(t) = (-1/tau)*x(t) + (1/tau)*u(t)
///           discretized system (ZoH):
///             x(k+1) = exp(samplingTime*(-1/tau))*x(k) + (1 - exp(samplingTime*(-1/tau))) * u(k) // NOLINT
template <typename T>
class FirstOrderFilter {
  public:
  FirstOrderFilter(double _timeConstantUp, double _timeConstantDown, T _initialState): // NOLINT
      timeConstantUp(_timeConstantUp),
      timeConstantDown(_timeConstantDown),
      previousState(_initialState) {}

  /// \brief    This method will apply a first order filter on the _inputState.
  T UpdateFilter(T _inputState, double _samplingTime)
  {
    T outputState;
    if (_inputState > previousState) {
      // Calcuate the outputState if accelerating.
      double alphaUp = exp(-_samplingTime / timeConstantUp);
      // x(k+1) = Ad*x(k) + Bd*u(k)
      outputState = alphaUp * previousState + (1 - alphaUp) * _inputState;
    }
    else
    {
      // Calculate the outputState if decelerating.
      double alphaDown = exp(-_samplingTime / timeConstantDown);
      outputState = alphaDown * previousState + (1 - alphaDown) * _inputState;
    }
    previousState = outputState;
    return outputState;
  }

  ~FirstOrderFilter() = default;

  protected:
  double timeConstantUp;
  double timeConstantDown;
  T previousState;
};

using namespace gz;
using namespace sim;
using namespace systems;

/// \brief Constants for specifying clockwise (kCw) and counter-clockwise (kCcw)
/// directions of rotation.
namespace turning_direction {
static const int kCcw = 1;
static const int kCw = -1;
}  // namespace turning_direction

/// \brief Type of input command to motor.
enum class MotorType {
  kVelocity,
  kPosition,
  kForce
};

class gz::sim::systems::DynamicThrustPropellerModelPrivate
{
  /// \brief Callback for actuator commands.
  public: void OnActuatorMsg(const msgs::Actuators &_msg);

  /// \brief Apply link forces and moments based on propeller state.
  public: void UpdateForcesAndMoments(EntityComponentManager &_ecm);

  /// \brief Joint Entity
  public: Entity jointEntity;

  /// \brief Joint name
  public: std::string jointName;

  /// \brief Link Entity
  public: Entity linkEntity;

  /// \brief Link name
  public: std::string linkName;

  /// \brief Parent link Entity
  public: Entity parentLinkEntity;

  /// \brief Parent link name
  public: std::string parentLinkName;

  /// \brief Model interface
  public: Model model{kNullEntity};

  /// \brief Topic for actuator commands.
  public: std::string commandSubTopic;

  /// \brief Topic namespace.
  public: std::string robotNamespace;

  /// \brief Sampling time (from motor_model.hpp).
  public: double samplingTime = 0.01;

  /// \brief Index of motor in Actuators msg on multirotor_base.
  public: int actuatorNumber = 0;

  /// \brief Turning direction of the motor.
  public: int turningDirection = turning_direction::kCw;

  /// \brief Type of input command to motor.
  public: MotorType motorType = MotorType::kVelocity;

  /// \brief air density
  /// at 25 deg C it's about 1.1839 kg/m^3
  /// At 20 °C and 101.325 kPa, dry air has a density of 1.2041 kg/m3.
  public: double airDensity = 1.2041;

  /// \brief Maximum rotational velocity command with units of rad/s.
  /// The default value is taken from gazebo_motor_model.h
  /// and is approximately 8000 revolutions / minute (rpm).
  public: double maxRotVelocity = 838.0;

  /// \brief Moment constant for computing drag torque based on thrust
  /// with units of length (m).
  /// The default value is taken from gazebo_motor_model.h
  public: double momentConstant = 0.016;

  /// \brief Propeller diameter with units of meter (converted from inches).
  public: double propellerDiameter = 0.1; 

  /// \brief Propeller pitch with units of meter (converted from inches).
  public: double propellerPitch = 0.01; 

  /// \brief Reference input to motor. For MotorType kVelocity, this
  /// is the reference angular velocity in rad/s.
  public: double refMotorInput = 0.0;

  /// \brief Rotor Vertical Velocity Drag Coefficient
  /// This drag model can be found Edward Liu's Dissertation (UTA-MAE 2023)
  public: double rotorVerticalVelocityDragCoefficient = 0;

  /// \brief Rotor Horizontal Velocity Drag Coefficient
  /// This drag model can be found Edward Liu's Dissertation (UTA-MAE 2023)
  public: double rotorHorizontalVelocityDragCoefficient = 0;

  /// \brief Rotor drag coefficient for propeller with units of N / (m/s^2).
  /// The default value is taken from gazebo_motor_model.h
  public: double rotorDragCoefficient = 1.0e-4;

  /// \brief Large joint velocities can cause problems with aliasing,
  /// so the joint velocity used by the physics engine is reduced
  /// this factor, while the larger value is used for computing
  /// propeller thrust.
  /// The default value is taken from gazebo_motor_model.h
  public: double rotorVelocitySlowdownSim = 10.0;

  /// \brief Time constant for rotor deceleration.
  /// The default value is taken from gazebo_motor_model.h
  public: double timeConstantDown = 1.0 / 40.0;

  /// \brief Time constant for rotor acceleration.
  /// The default value is taken from gazebo_motor_model.h
  public: double timeConstantUp = 1.0 / 80.0;

  /// \brief Filter on rotor velocity that has different time constants
  /// for increasing and decreasing values.
  public: std::unique_ptr<FirstOrderFilter<double>> rotorVelocityFilter;

  /// \brief Received Actuators message. This is nullopt if no message has been
  /// received.
  public: std::optional<msgs::Actuators> recvdActuatorsMsg;

  /// \brief Mutex to protect recvdActuatorsMsg.
  public: std::mutex recvdActuatorsMsgMutex;

  /// \brief Gazebo communication node.
  public: transport::Node node;
};

//////////////////////////////////////////////////
DynamicThrustPropellerModel::DynamicThrustPropellerModel()
  : dataPtr(std::make_unique<DynamicThrustPropellerModelPrivate>())
{
}

//////////////////////////////////////////////////
void DynamicThrustPropellerModel::Configure(const Entity &_entity,
    const std::shared_ptr<const sdf::Element> &_sdf,
    EntityComponentManager &_ecm,
    EventManager &/*_eventMgr*/)
{
  this->dataPtr->model = Model(_entity);

  if (!this->dataPtr->model.Valid(_ecm))
  {
    gzerr << "DynamicThrustPropellerModel plugin should be attached to a model "
           << "entity. Failed to initialize." << std::endl;
    return;
  }

  auto sdfClone = _sdf->Clone();

  this->dataPtr->robotNamespace.clear();

  if (sdfClone->HasElement("robotNamespace"))
  {
    this->dataPtr->robotNamespace =
        sdfClone->Get<std::string>("robotNamespace");
  }
  else
  {
    gzwarn << "No robotNamespace set using entity name.\n";
    this->dataPtr->robotNamespace = this->dataPtr->model.Name(_ecm);
  }

  // Get params from SDF
  if (sdfClone->HasElement("jointName"))
  {
    this->dataPtr->jointName = sdfClone->Get<std::string>("jointName");
  }

  if (this->dataPtr->jointName.empty())
  {
    gzerr << "DynamicThrustPropellerModel found an empty jointName parameter. "
           << "Failed to initialize.";
    return;
  }

  if (sdfClone->HasElement("linkName"))
  {
    this->dataPtr->linkName = sdfClone->Get<std::string>("linkName");
  }

  if (this->dataPtr->linkName.empty())
  {
    gzerr << "DynamicThrustPropellerModel found an empty linkName parameter. "
           << "Failed to initialize.";
    return;
  }

  if (sdfClone->HasElement("actuator_number"))
  {
    this->dataPtr->actuatorNumber =
      sdfClone->GetElement("actuator_number")->Get<int>();
  }
  else if (sdfClone->HasElement("motorNumber"))
  {
    this->dataPtr->actuatorNumber =
      sdfClone->GetElement("motorNumber")->Get<int>();
    gzwarn << "<motorNumber> is depricated, "
           << "please use <actuator_number>.\n";
  }
  else
  {
    gzerr << "Please specify a actuator_number.\n";
  }

  if (sdfClone->HasElement("turningDirection"))
  {
    auto turningDirection =
        sdfClone->GetElement("turningDirection")->Get<std::string>();
    if (turningDirection == "cw")
      this->dataPtr->turningDirection = turning_direction::kCw;
    else if (turningDirection == "ccw")
      this->dataPtr->turningDirection = turning_direction::kCcw;
    else
      gzerr << "Please only use 'cw' or 'ccw' as turningDirection.\n";
  }
  else
  {
    gzerr << "Please specify a turning direction ('cw' or 'ccw').\n";
  }

  if (sdfClone->HasElement("motorType"))
  {
    auto motorType = sdfClone->GetElement("motorType")->Get<std::string>();
    if (motorType == "velocity")
      this->dataPtr->motorType = MotorType::kVelocity;
    else if (motorType == "position")
    {
      this->dataPtr->motorType = MotorType::kPosition;
      gzerr << "motorType 'position' not supported" << std::endl;
    }
    else if (motorType == "force")
    {
      this->dataPtr->motorType = MotorType::kForce;
      gzerr << "motorType 'force' not supported" << std::endl;
    }
    else
    {
      gzerr << "Please only use 'velocity', 'position' or "
               "'force' as motorType.\n";
    }
  }
  else
  {
    gzwarn << "motorType not specified, using velocity.\n";
    this->dataPtr->motorType = MotorType::kVelocity;
  }

  sdfClone->Get<std::string>("commandSubTopic",
      this->dataPtr->commandSubTopic, this->dataPtr->commandSubTopic);

  sdfClone->Get<double>("rotorDragCoefficient",
      this->dataPtr->rotorDragCoefficient,
      this->dataPtr->rotorDragCoefficient);
  sdfClone->Get<double>("rotorVerticalVelocityDragCoefficient",
      this->dataPtr->rotorVerticalVelocityDragCoefficient,
      this->dataPtr->rotorVerticalVelocityDragCoefficient);
  sdfClone->Get<double>("rotorHorizontalVelocityDragCoefficient",
      this->dataPtr->rotorHorizontalVelocityDragCoefficient,
      this->dataPtr->rotorHorizontalVelocityDragCoefficient);
  sdfClone->Get<double>("maxRotVelocity",
      this->dataPtr->maxRotVelocity, this->dataPtr->maxRotVelocity);
  sdfClone->Get<double>("propellerDiameter",
      this->dataPtr->propellerDiameter, this->dataPtr->propellerDiameter);
  sdfClone->Get<double>("propellerPitch",
      this->dataPtr->propellerPitch, this->dataPtr->propellerPitch);
  sdfClone->Get<double>("momentConstant",
      this->dataPtr->momentConstant, this->dataPtr->momentConstant);

  sdfClone->Get<double>("timeConstantUp",
      this->dataPtr->timeConstantUp, this->dataPtr->timeConstantUp);
  sdfClone->Get<double>("timeConstantDown",
      this->dataPtr->timeConstantDown, this->dataPtr->timeConstantDown);
  sdfClone->Get<double>("rotorVelocitySlowdownSim",
      this->dataPtr->rotorVelocitySlowdownSim, 10);

  // Create the first order filter.
  this->dataPtr->rotorVelocityFilter =
      std::make_unique<FirstOrderFilter<double>>(
          this->dataPtr->timeConstantUp, this->dataPtr->timeConstantDown,
          this->dataPtr->refMotorInput);

  // Subscribe to actuator command messages
  std::string topic = transport::TopicUtils::AsValidTopic(
      this->dataPtr->robotNamespace + "/" + this->dataPtr->commandSubTopic);
  if (topic.empty())
  {
    gzerr << "Failed to create topic for [" << this->dataPtr->robotNamespace
           << "]" << std::endl;
    return;
  }
  else
  {
    gzdbg << "Listening to topic: " << topic << std::endl;
  }
  this->dataPtr->node.Subscribe(topic,
      &DynamicThrustPropellerModelPrivate::OnActuatorMsg, this->dataPtr.get());
}

//////////////////////////////////////////////////
void DynamicThrustPropellerModel::PreUpdate(const UpdateInfo &_info,
    EntityComponentManager &_ecm)
{
  GZ_PROFILE("DynamicThrustPropellerModel::PreUpdate");

  // \TODO(anyone) Support rewind
  if (_info.dt < std::chrono::steady_clock::duration::zero())
  {
    gzwarn << "Detected jump back in time ["
        << std::chrono::duration_cast<std::chrono::seconds>(_info.dt).count()
        << "s]. System may not work properly." << std::endl;
  }

  // If the joint or links haven't been identified yet, look for them
  if (this->dataPtr->jointEntity == kNullEntity)
  {
    this->dataPtr->jointEntity =
        this->dataPtr->model.JointByName(_ecm, this->dataPtr->jointName);

    const auto parentLinkName = _ecm.Component<components::ParentLinkName>(
        this->dataPtr->jointEntity);
    this->dataPtr->parentLinkName = parentLinkName->Data();
  }

  if (this->dataPtr->linkEntity == kNullEntity)
  {
    this->dataPtr->linkEntity =
        this->dataPtr->model.LinkByName(_ecm, this->dataPtr->linkName);
  }

  if (this->dataPtr->parentLinkEntity == kNullEntity)
  {
    this->dataPtr->parentLinkEntity =
        this->dataPtr->model.LinkByName(_ecm, this->dataPtr->parentLinkName);
  }

  if (this->dataPtr->jointEntity == kNullEntity ||
      this->dataPtr->linkEntity == kNullEntity ||
      this->dataPtr->parentLinkEntity == kNullEntity)
    return;

  // skip UpdateForcesAndMoments if needed components are missing
  bool doUpdateForcesAndMoments = true;

  const auto jointVelocity = _ecm.Component<components::JointVelocity>(
      this->dataPtr->jointEntity);
  if (!jointVelocity)
  {
    _ecm.CreateComponent(this->dataPtr->jointEntity,
        components::JointVelocity());
    doUpdateForcesAndMoments = false;
  }
  else if (jointVelocity->Data().empty())
  {
    doUpdateForcesAndMoments = false;
  }

  if (!_ecm.Component<components::JointVelocityCmd>(
      this->dataPtr->jointEntity))
  {
    _ecm.CreateComponent(this->dataPtr->jointEntity,
        components::JointVelocityCmd({0}));
    doUpdateForcesAndMoments = false;
  }

  if (!_ecm.Component<components::WorldPose>(this->dataPtr->linkEntity))
  {
    _ecm.CreateComponent(this->dataPtr->linkEntity, components::WorldPose());
    doUpdateForcesAndMoments = false;
  }
  if (!_ecm.Component<components::WorldLinearVelocity>(
      this->dataPtr->linkEntity))
  {
    _ecm.CreateComponent(this->dataPtr->linkEntity,
        components::WorldLinearVelocity());
    doUpdateForcesAndMoments = false;
  }

  if (!_ecm.Component<components::WorldPose>(this->dataPtr->parentLinkEntity))
  {
    _ecm.CreateComponent(this->dataPtr->parentLinkEntity,
        components::WorldPose());
    doUpdateForcesAndMoments = false;
  }

  // Nothing left to do if paused.
  if (_info.paused)
    return;

  this->dataPtr->samplingTime =
    std::chrono::duration<double>(_info.dt).count();
  if (doUpdateForcesAndMoments)
  {
    this->dataPtr->UpdateForcesAndMoments(_ecm);
  }
}

//////////////////////////////////////////////////
void DynamicThrustPropellerModelPrivate::OnActuatorMsg(
    const msgs::Actuators &_msg)
{
  std::lock_guard<std::mutex> lock(this->recvdActuatorsMsgMutex);
  this->recvdActuatorsMsg = _msg;
}

//////////////////////////////////////////////////
void DynamicThrustPropellerModelPrivate::UpdateForcesAndMoments(
    EntityComponentManager &_ecm)
{
  GZ_PROFILE("DynamicThrustPropellerModelPrivate::UpdateForcesAndMoments");

  std::optional<msgs::Actuators> msg;
  auto actuatorMsgComp =
      _ecm.Component<components::Actuators>(this->model.Entity());

  // Actuators messages can come in from transport or via a component. If a
  // component is available, it takes precedence.
  if (actuatorMsgComp)
  {
    msg = actuatorMsgComp->Data();
  }
  else
  {
    std::lock_guard<std::mutex> lock(this->recvdActuatorsMsgMutex);
    if (this->recvdActuatorsMsg.has_value())
    {
      msg = *this->recvdActuatorsMsg;
      this->recvdActuatorsMsg.reset();
    }
  }

  if (msg.has_value())
  {
    if (this->actuatorNumber > msg->velocity_size() - 1)
    {
      gzerr << "You tried to access index " << this->actuatorNumber
        << " of the Actuator velocity array which is of size "
        << msg->velocity_size() << std::endl;
      return;
    }

    if (this->motorType == MotorType::kVelocity)
    {
      this->refMotorInput = std::min(
          static_cast<double>(msg->velocity(this->actuatorNumber)),
          static_cast<double>(this->maxRotVelocity));
    }
    //  else if (this->motorType == MotorType::kPosition)
    else  // if (this->motorType == MotorType::kForce) {
    {
      this->refMotorInput = msg->velocity(this->actuatorNumber);
    }
  }

  switch (this->motorType)
  {
    case (MotorType::kPosition):
    {
      // double err = joint_->GetAngle(0).Radian() - this->refMotorInput;
      // double force = pids_.Update(err, this->samplingTime);
      // joint_->SetForce(0, force);
      break;
    }
    case (MotorType::kForce):
    {
      // joint_->SetForce(0, this->refMotorInput);
      break;
    }
    default:  // MotorType::kVelocity
    {
      const auto jointVelocity = _ecm.Component<components::JointVelocity>(
          this->jointEntity);
      double motorRotVel = jointVelocity->Data()[0];
      if (motorRotVel / (2 * GZ_PI) > 1 / (2 * this->samplingTime))
      {
        gzerr << "Aliasing on motor [" << this->actuatorNumber
              << "] might occur. Consider making smaller simulation time "
                 "steps or raising the rotorVelocitySlowdownSim param.\n";
      }
      double realMotorVelocity =
          motorRotVel * this->rotorVelocitySlowdownSim;

      int realMotorVelocitySign =
          (realMotorVelocity > 0) - (realMotorVelocity < 0);

      double motorRPM = abs(realMotorVelocity) * 60 / ( 2 * GZ_PI );
      

      using Pose = math::Pose3d;
      using Vector3 = math::Vector3d;

      Link link(this->linkEntity);
      const auto worldPose = link.WorldPose(_ecm);

      

      const auto jointPose = _ecm.Component<components::Pose>(
          this->jointEntity);
      if (!jointPose)
      {
        gzerr << "joint " << this->jointName << " has no Pose"
               << "component" << std::endl;
        return;
      }
      // compute joint world pose by multiplying child link WorldPose
      // with joint Pose
      Pose jointWorldPose = *worldPose * jointPose->Data();

      const auto jointAxisComp = _ecm.Component<components::JointAxis>(
          this->jointEntity);
      if (!jointAxisComp)
      {
        gzerr << "joint " << this->jointName << " has no JointAxis"
               << "component" << std::endl;
        return;
      }

      const auto worldLinearVel = link.WorldLinearVelocity(_ecm);

      Entity windEntity = _ecm.EntityByComponents(components::Wind());
      auto windLinearVel =
          _ecm.Component<components::WorldLinearVelocity>(windEntity);
      Vector3 windSpeedWorld = windLinearVel->Data();


      // Forces from (Edward Liu Tsung UTA-MAE Dissertation 2023)
      // Also the default plugin resources are the same as Edward's model
      // which is below:
      // Forces from Philppe Martin's and Erwan Salaun's
      // 2010 IEEE Conference on Robotics and Automation paper
      // The True Role of Accelerometer Feedback in Quadrotor Control
      // - \omega * \lambda_1 * V_A^{\perp}
      Vector3 jointAxis =
          jointWorldPose.Rot().RotateVector(jointAxisComp->Data().Xyz());

      Vector3 bodyVelocityWorld = *worldLinearVel;

      Vector3 relativeWindVelocityWorld = bodyVelocityWorld - windSpeedWorld;

      Vector3 bodyVelocityPerpendicular =
          relativeWindVelocityWorld -
          (relativeWindVelocityWorld.Dot(jointAxis) * jointAxis);

      Vector3 airDrag = -std::abs(realMotorVelocity) *
                               this->rotorDragCoefficient *
                               bodyVelocityPerpendicular;

                      
      Vector3 bodyVelocityParallel = (relativeWindVelocityWorld.Dot(jointAxis) * jointAxis);
      
      double V0 = bodyVelocityParallel.Length();

      // Assuming symmetric propellers (or rotors) for the thrust calculation.
      double thrust = this->turningDirection * realMotorVelocitySign *
                      this->airDensity *  0.25 * GZ_PI * (this->propellerDiameter*this->propellerDiameter) *
                      ( (pow(motorRPM * this->propellerPitch / 60 , 2)) - ((motorRPM * this->propellerPitch / 60)*V0) ) *
                      ( pow(this->propellerDiameter/(3.29546 * this->propellerPitch) ,1.5) );

      double yaw_moment = -this->turningDirection * thrust * this->momentConstant;


      // Apply a force to the link.
      link.AddWorldForce(_ecm,
                         worldPose->Rot().RotateVector(Vector3(thrust, 0, 0)));

      // Apply air drag to link.
      link.AddWorldForce(_ecm, airDrag);
      // Moments get the parent link, such that the resulting torques can be
      // applied.
      Vector3 parentWorldTorque;
      auto parentWrenchComp =
        _ecm.Component<components::ExternalWorldWrenchCmd>(
          this->parentLinkEntity);
      // gazebo_motor_model.cpp subtracts the GetWorldCoGPose() of the
      // child link from the parent but only uses the rotation component.
      // Since GetWorldCoGPose() uses the link frame orientation, it
      // is equivalent to use WorldPose().Rot().
      Link parentLink(this->parentLinkEntity);
      const auto parentWorldPose = parentLink.WorldPose(_ecm);
      // The transformation from the parent_link to the link_.
      // Pose poseDifference =
      //  parent_links.at(0)->GetWorldCoGPose().Inverse()
      //  * link_->GetWorldCoGPose()
      Pose poseDifference = (*parentWorldPose).Inverse() * (*worldPose);
      Vector3 dragTorque( 0, 0,  yaw_moment);
      // Transforming the drag torque into the parent frame to handle
      // arbitrary rotor orientations.
      Vector3 dragTorqueParentFrame =
          poseDifference.Rot().RotateVector(dragTorque);
      parentWorldTorque =
          parentWorldPose->Rot().RotateVector(dragTorqueParentFrame);


      //gzerr << "link: " << this->linkName << " refInput: " << this->refMotorInput << " MotVel: " << realMotorVelocity << " thrust: " << thrust << " V0: " << V0 << " RPM: " << motorRPM   << std::endl;


      if (!parentWrenchComp)
      {
        components::ExternalWorldWrenchCmd wrench;
        msgs::Set(wrench.Data().mutable_torque(), parentWorldTorque);
        _ecm.CreateComponent(this->parentLinkEntity, wrench);
      }
      else
      {
        msgs::Set(parentWrenchComp->Data().mutable_torque(),
          msgs::Convert(parentWrenchComp->Data().torque()) + parentWorldTorque);
      }
      // Apply the filter on the motor's velocity.
      double refMotorRotVel;
      refMotorRotVel = this->rotorVelocityFilter->UpdateFilter(
          this->refMotorInput, this->samplingTime);

      const auto jointVelCmd = _ecm.Component<components::JointVelocityCmd>(
          this->jointEntity);
      *jointVelCmd = components::JointVelocityCmd(
          {this->turningDirection * refMotorRotVel
                              / this->rotorVelocitySlowdownSim});
    }
  }
}

GZ_ADD_PLUGIN(DynamicThrustPropellerModel,
                    System,
                    DynamicThrustPropellerModel::ISystemConfigure,
                    DynamicThrustPropellerModel::ISystemPreUpdate)

GZ_ADD_PLUGIN_ALIAS(DynamicThrustPropellerModel,
                          "gz::sim::systems::DynamicThrustPropellerModel")

// TODO(CH3): Deprecated, remove on version 8
GZ_ADD_PLUGIN_ALIAS(DynamicThrustPropellerModel,
                          "ignition::gazebo::systems::DynamicThrustPropellerModel")
