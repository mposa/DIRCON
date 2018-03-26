#include <memory>

#include <gflags/gflags.h>
#include "drake/multibody/rigid_body_tree_construction.h"
#include "drake/multibody/joints/floating_base_types.h"
#include "drake/multibody/parsers/urdf_parser.h"
#include "drake/multibody/rigid_body_tree.h"
#include "drake/multibody/kinematics_cache.h"
#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"
#include "drake/solvers/snopt_solver.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/trajectory_source.h"
#include "drake/multibody/rigid_body_plant/drake_visualizer.h"
#include "drake/lcm/drake_lcm.h"
#include "dircon_position_data.h"
#include "dircon_kinematic_data_set.h"
#include "dircon.h"

using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using Eigen::Matrix3Xd;
using std::cout;
using std::endl;
using drake::systems::trajectory_optimization::Dircon;
using drake::systems::trajectory_optimization::DirconDynamicConstraint;
using drake::systems::trajectory_optimization::DirconKinematicConstraint;
using drake::trajectories::PiecewisePolynomial;



//template VectorXd RigidBodyTree<double>::transformPointsJacobianDotTimesV<double, Matrix3Xd>(KinematicsCache<double> const&, Eigen::MatrixBase<Matrix3Xd> const&, int, int);

namespace drake{
namespace goldilocks {
namespace examples {
namespace {

int testConstraints() {

  //auto model = std::make_unique<RigidBodyTree<double>>();
  RigidBodyTree<double> tree;
  parsers::urdf::AddModelInstanceFromUrdfFileToWorld("../../examples/Acrobot/Acrobot.urdf", multibody::joints::kFixed, &tree);
  //const std::unique_ptr<const RigidBodyTree<double>> tree =  std::unique_ptr<const RigidBodyTree<double>>(&model);

  int bodyIdx = 5;
  Vector3d pt;
  pt << 0,0,0;
  Vector3d pt2;
  pt2 << 0,0,0;
  bool isXZ = true;
  DirconPositionData<AutoDiffXd> constraintd = DirconPositionData<AutoDiffXd>(tree,bodyIdx,pt,isXZ);
  DirconPositionData<AutoDiffXd> constraintd2 = DirconPositionData<AutoDiffXd>(tree,bodyIdx,pt2,isXZ);
  DirconPositionData<double> constraint = DirconPositionData<double>(tree,bodyIdx,pt,isXZ);
  DirconPositionData<double> constraint2 = DirconPositionData<double>(tree,bodyIdx,pt2,isXZ);
  cout << "??" <<endl;
  int n = 4;
  int nl = 4;
  int nu = 1;

  VectorXd q(n,1);
  VectorXd v(n,1);
  q << 0,0,M_PI,0;
  v << 0,0,0,0;
  VectorXd x(2*n,1);
  x << q, v;
  VectorXd u(nu,1);
  u << 0;
  VectorXd l(nl,1);
  l << 0, 2*9.81, 0, 0;

  VectorXd q1(n,1);
  VectorXd v1(n,1);
  q1 << 0,0,M_PI,0;
  v1 << 0,0,0,0;
  VectorXd x1(2*n,1);
  x1 << q1, v1;
  VectorXd u1(nu,1);
  u1 << 0;
  VectorXd l1(nl,1);
  l1 << 0,2*9.81,0,0;
  VectorXd lc(nl,1);
  lc << 0,2*9.81,0,0;
  VectorXd vc(nl,1);
  vc << 0, 0, 0, 0;

  VectorXd h(1,1);
  h << .1;

  VectorXd vars(1+4*n + 2*nu + 4*nl);
  vars << h,x,x1,u,u1,l,l1,lc,vc;

  AutoDiffVecXd vars_autodiff = math::initializeAutoDiff(vars);
  AutoDiffVecXd x_autodiff = vars_autodiff.segment(1,2*n);  
  AutoDiffVecXd q_autodiff = x_autodiff.head(n);
  AutoDiffVecXd v_autodiff = x_autodiff.tail(n);

  AutoDiffVecXd u_autodiff = vars_autodiff.segment(1+4*n, nu);
  AutoDiffVecXd l_autodiff = vars_autodiff.segment(1+4*n+2*nu, nl);

  std::vector<DirconKinematicData<AutoDiffXd>*> constraintsd;
  constraintsd.push_back(&constraintd);
  constraintsd.push_back(&constraintd2);
  auto datasetd = DirconKinematicDataSet<AutoDiffXd>(tree, &constraintsd);

  //auto dataset_ptr =  std::unique_ptr<DirconKinematicDataSet<AutoDiffXd>>(&datasetd);

   datasetd.updateData(x_autodiff, u_autodiff, l_autodiff);

  auto dynamicConstraint = std::make_shared<DirconDynamicConstraint>(tree, datasetd);
  auto kinematicConstraint = std::make_shared<DirconKinematicConstraint>(tree, datasetd);


  //AutoDiffVecXd xul 
  //VectorXd y(2*n);
  AutoDiffVecXd x_dynamic = vars_autodiff;
  AutoDiffVecXd y_dynamic;// = y.template cast<AutoDiffXd>();
  //std::cout << dynamicY << endl;
  //std::cout << x_dynamic.rows() << " " << dynamicConstraint.num_vars() << endl;
  dynamicConstraint->Eval(x_dynamic,y_dynamic);

  cout << "*********** x_dynamic  ***********" << endl;
  std::cout << x_dynamic << endl;
  cout << "*********** y_dynamic  ***********" << endl;
  std::cout << y_dynamic << endl;
  cout << "*********** dy_dynamic  ***********" << endl;
  std::cout << math::autoDiffToGradientMatrix(y_dynamic) << endl;

  AutoDiffVecXd x_kinematic = VectorX<AutoDiffXd>(2*n+nu+nl);
  x_kinematic << x_autodiff, u_autodiff, l_autodiff;
  AutoDiffVecXd y_kinematic;

  kinematicConstraint->Eval(x_kinematic,y_kinematic);

  cout << "*********** c  ***********" << endl;
  std::cout << datasetd.getC() << endl;


  cout << "*********** xdot  ***********" << endl;
  std::cout << datasetd.getXDot() << endl;

  cout << "*********** x  ***********" << endl;
  std::cout << x_autodiff << endl;
  cout << "*********** u  ***********" << endl;
  std::cout << u_autodiff << endl;
  cout << "*********** l  ***********" << endl;
  std::cout << l_autodiff << endl;
  cout << "*********** y_kinematic  ***********" << endl;
  std::cout << y_kinematic << endl;
  cout << "*********** dy_kinematic  ***********" << endl;
  std::cout << math::autoDiffToGradientMatrix(y_kinematic) << endl;

  return 0;
}

int testDircon() {
RigidBodyTree<double> tree;
  parsers::urdf::AddModelInstanceFromUrdfFileToWorld("../../examples/Acrobot/Acrobot.urdf", multibody::joints::kFixed, &tree);
  //const std::unique_ptr<const RigidBodyTree<double>> tree =  std::unique_ptr<const RigidBodyTree<double>>(&model);

  cout << tree.getBodyOrFrameName(0) << endl;
  cout << tree.getBodyOrFrameName(1) << endl;
  cout << tree.getBodyOrFrameName(2) << endl;
  cout << tree.getBodyOrFrameName(3) << endl;
  cout << tree.getBodyOrFrameName(4) << endl;
  cout << tree.getBodyOrFrameName(5) << endl;
  int n = 4;
  int nu = 1;
  int nl = 2;
  int bodyIdx = 4;
  Vector3d pt;
  pt << 0,0,0;
  bool isXZ = true;
  DirconPositionData<AutoDiffXd> constraint = DirconPositionData<AutoDiffXd>(tree,bodyIdx,pt,isXZ);

  std::vector<DirconKinematicData<AutoDiffXd>*> constraints;
  constraints.push_back(&constraint);
  auto dataset = DirconKinematicDataSet<AutoDiffXd>(tree, &constraints);

  int N = 10;
  auto trajopt = std::make_shared<Dircon>(tree, N, .01, 3.0, dataset, DirconOptions(dataset.getNumConstraints()));

  trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(), "Print file","snopt.out");

  //Construct what should be a feasible trajectory
  std::vector<double> init_time;
  std::vector<MatrixXd> init_x;
  std::vector<MatrixXd> init_u;
  std::vector<MatrixXd> init_l;
  std::vector<MatrixXd> init_lc;
  std::vector<MatrixXd> init_vc;

  VectorXd init_l_vec(nl);
  init_l_vec << 0, tree.getMass()*9.81;

  for (int i = 0; i < N; i++) {
    init_time.push_back(i*.1);
    init_x.push_back(VectorXd::Zero(2*n));
    init_x[i](2) = M_PI;
    init_u.push_back(VectorXd::Zero(nu));
    //init_u[i](0) = .1;
    init_l.push_back(init_l_vec);
    init_lc.push_back(init_l_vec);
    init_vc.push_back(VectorXd::Zero(nl));
  }

  auto init_x_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time,init_x);
  auto init_u_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time,init_u);
  auto init_l_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time,init_l);
  auto init_lc_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time,init_lc);
  auto init_vc_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time,init_vc);
  //trajopt->SetInitialTrajectory(init_u_traj,init_x_traj,init_l_traj,init_lc_traj,init_vc_traj);

  Eigen::VectorXd x0(2*n);
  x0 << 0, 0, 0, 0, 0, 0, 0, 0;
  Eigen::VectorXd xG(2*n);
  xG << 0, 0, M_PI, 0, 0, 0, 0, 0;
  trajopt->AddLinearConstraint(trajopt->initial_state() == x0);
  trajopt->AddLinearConstraint(trajopt->final_state() == xG);

  const double kTorqueLimit = 8;
  auto u = trajopt->input();
  trajopt->AddConstraintToAllKnotPoints(-kTorqueLimit <= u(0));
  trajopt->AddConstraintToAllKnotPoints(u(0) <= kTorqueLimit);

  const double R = 10;  // Cost on input "effort".
  trajopt->AddRunningCost((R * u) * u);

  trajopt->AddEqualTimeIntervalsConstraints();

  auto result = trajopt->Solve();
  trajopt->PrintSolution();

  //visualizer
  lcm::DrakeLcm lcm;
  systems::DiagramBuilder<double> builder;
  const trajectories::PiecewisePolynomial<double> pp_xtraj = trajopt->ReconstructStateTrajectory();
  auto state_source = builder.AddSystem<systems::TrajectorySource>(pp_xtraj);
  auto publisher = builder.AddSystem<systems::DrakeVisualizer>(tree, &lcm);
  publisher->set_publish_period(1.0 / 60.0);
  builder.Connect(state_source->get_output_port(),
                  publisher->get_input_port(0));

  auto diagram = builder.Build();

  systems::Simulator<double> simulator(*diagram);

  simulator.set_target_realtime_rate(1);
  simulator.Initialize();
  simulator.StepTo(pp_xtraj.end_time());
  return 0;

}



}  // namespace
}  // namespace examples
}  // namespace goldilocks
}  // namespace drake

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  //drake::goldilocks::examples::testConstraints();
  drake::goldilocks::examples::testDircon();
  return 0;
}