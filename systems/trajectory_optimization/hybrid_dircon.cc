#include "hybrid_dircon.h"

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"

namespace drake {
namespace systems {
namespace trajectory_optimization {

using solvers::Binding;
using solvers::Constraint;
using solvers::MathematicalProgram;
using solvers::VectorXDecisionVariable;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using Eigen::Map;
using std::vector;


template <typename T>
HybridDircon<T>::HybridDircon(const RigidBodyTree<double>& tree, vector<int> num_time_samples, vector<double> minimum_timestep,
  vector<double> maximum_timestep, vector<DirconKinematicDataSet<T>*> constraints, vector<DirconOptions> options)
    : MultipleShooting(tree.get_num_actuators(), tree.get_num_positions() + tree.get_num_velocities(), 
      std::accumulate(num_time_samples.begin(), num_time_samples.end(),0), 1e-8, 1e8),
      num_modes_(num_time_samples.size()),
      mode_lengths_(num_time_samples) {

  DRAKE_ASSERT(minimum_timestep.size() == num_modes_);
  DRAKE_ASSERT(maximum_timestep.size() == num_modes_);
  DRAKE_ASSERT(constraints.size() == num_modes_);

  tree_ = &tree;
  constraints_ = constraints;

  //Initialization is looped over the modes
  int counter = 0;
  for (int i = 0; i < num_modes_; i++) {
    mode_start_.push_back(counter);

    //set timestep bounds
    for (int j = 0; j < mode_lengths_[i]; j++) {
      AddBoundingBoxConstraint(minimum_timestep[i], maximum_timestep[i], timestep(j));
    }
    for (int j = 0; j < mode_lengths_[i] - 1; j++) {
      AddLinearConstraint(timestep(j) == timestep(j+1)); //all timesteps must be equal
    }

    //initialize constraint lengths
    num_kinematic_constraints_.push_back(constraints_[i]->countConstraints());

    //initialize decision variables
    force_vars_.push_back(NewContinuousVariables(constraints_[i]->countConstraints() * num_time_samples[i], "lambda[" + std::to_string(i) + "]"));
    collocation_force_vars_.push_back(NewContinuousVariables(constraints_[i]->countConstraints() * (num_time_samples[i] - 1), "lambda_c[" + std::to_string(i) + "]"));
    collocation_slack_vars_.push_back(NewContinuousVariables(constraints_[i]->countConstraints() * (num_time_samples[i] - 1), "v_c[" + std::to_string(i) + "]"));
    offset_vars_.push_back(NewContinuousVariables(options[i].getNumRelative(), "offset[" + std::to_string(i) + "]"));
    v_post_impact_vars_ = NewContinuousVariables(tree.get_num_velocities(), "v_p");

    auto constraint = std::make_shared<DirconDynamicConstraint<T>>(tree, *constraints_[i]);

    DRAKE_ASSERT(static_cast<int>(constraint->num_constraints()) == num_states());

    // For N-1 timesteps, add a constraint which depends on the knot
    // value along with the state and input vectors at that knot and the
    // next.

    //TODO: To enable caching of constraint calculations, I probably need to make deep copies of constraints (and make another container
    // class that that has double the info for time i and i+1)

    //Adding dynamic constraints
    for (int j = 0; j < mode_lengths_[i] - 1; j++) {
      int time_index = mode_start_[i] + j;
      vector<solvers::VectorXDecisionVariable> x_next;
      AddConstraint(constraint,
                    {h_vars().segment(time_index,1),
                     state_vars_by_mode(i, j),
                     state_vars_by_mode(i, j+1),
                     u_vars().segment(time_index * num_inputs(), num_inputs() * 2),
                     force_vars(i).segment(j * num_kinematic_constraints(i), num_kinematic_constraints(i) * 2),
                     collocation_force_vars(i).segment(j * num_kinematic_constraints(i), num_kinematic_constraints(i)),
                     collocation_slack_vars(i).segment(j * num_kinematic_constraints(i), num_kinematic_constraints(i))});
    }

    //Adding kinematic constraints
    auto kinematic_constraint = std::make_shared<DirconKinematicConstraint<T>>(tree, *constraints_[i],
      options[i].getConstraintsRelative());
    for (int j = 1; j < mode_lengths_[i] - 1; j++) {
      int time_index = mode_start_[i] + j;
      AddConstraint(kinematic_constraint,
                    {state_vars_by_mode(i,j),
                     u_vars().segment(time_index * num_inputs(), num_inputs()),
                     force_vars(i).segment(j * num_kinematic_constraints(i), num_kinematic_constraints(i)),
                     offset_vars(i)});
    }

    //special case first and last tiemstep based on options
    auto kinematic_constraint_start = std::make_shared<DirconKinematicConstraint<T>>(tree, *constraints_[i],
      options[i].getConstraintsRelative(), options[i].getStartType());
    AddConstraint(kinematic_constraint_start,
                  {state_vars_by_mode(i,0),
                   u_vars().segment(mode_start_[i], num_inputs()),
                   force_vars(i).segment(0, num_kinematic_constraints(i)),
                   offset_vars(i)});


    auto kinematic_constraint_end = std::make_shared<DirconKinematicConstraint<T>>(tree, *constraints_[i],
      options[i].getConstraintsRelative(), options[i].getEndType());
    AddConstraint(kinematic_constraint_end,
                  {state_vars_by_mode(i, mode_lengths_[i] - 1),
                   u_vars().segment((mode_start_[i] + mode_lengths_[i] - 1) * num_inputs(), num_inputs()),
                   force_vars(i).segment((mode_lengths_[i]-1) * num_kinematic_constraints(i), num_kinematic_constraints(i)),
                   offset_vars(i)});


    //Add constraints on force variables
    for (int l = 0; l < mode_lengths_[i] - 1; l++) {
      int start_index = l*num_kinematic_constraints(i);
      for (int j = 0; j < constraints_[i]->getNumConstraintObjects(); j++) {
        DirconKinematicData<T>* constraint_j = constraints_[i]->getConstraint(j);
        start_index += constraint_j->getLength();
        for (int k = 0; k < constraint_j->numForceConstraints(); k++) {
          AddConstraint(constraint_j->getForceConstraint(k), force_vars(i).segment(start_index, constraint_j->getLength()));
        }
      }
    }

    //Force cost option
    if (options[i].getForceCost() != 0) {
      auto A = MatrixXd::Identity(num_kinematic_constraints(i),num_kinematic_constraints(i));
      auto b = MatrixXd::Zero(num_kinematic_constraints(i),1);
      for (int j=0; j <  mode_lengths_[i]; j++) {
        AddL2NormCost(A,b,force(i,j));
      }
    }

    //TODO: add impact constraints
    // add new decision variables for impulse
    // add new constraint and constraint type


    counter += mode_lengths_[i];
  }
}

template <typename T>
const Eigen::VectorBlock<solvers::VectorXDecisionVariable> HybridDircon<T>::v_post_impact_vars_by_mode(int mode) {
  return v_post_impact_vars_.segment(mode * tree_->get_num_velocities(), tree_->get_num_velocities());
}

template <typename T>
const Eigen::VectorBlock<solvers::VectorXDecisionVariable> HybridDircon<T>::state_vars_by_mode(int mode, int time_index)  {
  if (time_index == 0 && mode > 0) {
    solvers::VectorXDecisionVariable ret;
    ret << x_vars().segment(mode_start_[mode] + time_index, tree_->get_num_positions()),
          v_post_impact_vars_by_mode(mode);
    return Eigen::VectorBlock<solvers::VectorXDecisionVariable>(ret, 0, num_states());
  } else {
    solvers::VectorXDecisionVariable ret;
    ret << x_vars().segment(mode_start_[mode] + time_index, num_states());
    return Eigen::VectorBlock<solvers::VectorXDecisionVariable>(ret, 0, num_states());
  }
}

//TODO: need to configure this to handle the hybrid discontinuities properly
template <typename T>
void HybridDircon<T>::DoAddRunningCost(const symbolic::Expression& g) {
  // Trapezoidal integration:
  //    sum_{i=0...N-2} h_i/2.0 * (g_i + g_{i+1}), or
  // g_0*h_0/2.0 + [sum_{i=1...N-2} g_i*(h_{i-1} + h_i)/2.0] +
  // g_{N-1}*h_{N-2}/2.0.

  AddCost(0.5 * SubstitutePlaceholderVariables(g * h_vars()(0) / 2, 0));
  for (int i = 1; i < N() - 2; i++) {
    AddCost(SubstitutePlaceholderVariables(
        g * (h_vars()(i - 1) + h_vars()(i)) / 2, i));
  }
  AddCost(0.5 *
          SubstitutePlaceholderVariables(g * h_vars()(N() - 2) / 2, N() - 1));
}

template <typename T>
PiecewisePolynomial<double> HybridDircon<T>::ReconstructInputTrajectory()
    const {
  Eigen::VectorXd times = GetSampleTimes();
  vector<double> times_vec(N());
  vector<Eigen::MatrixXd> inputs(N());
  for (int i = 0; i < N(); i++) {
    times_vec[i] = times(i);
    inputs[i] = GetSolution(input(i));
  }
  return PiecewisePolynomial<double>::FirstOrderHold(times_vec, inputs);
}

//TODO: need to configure this to handle the hybrid discontinuities properly
template <typename T>
PiecewisePolynomial<double> HybridDircon<T>::ReconstructStateTrajectory()
    const {
  Eigen::VectorXd times = GetSampleTimes();
  vector<double> times_vec(N());
  vector<Eigen::MatrixXd> states(N());
  vector<Eigen::MatrixXd> inputs(N());
  vector<Eigen::MatrixXd> forces(N());
  vector<Eigen::MatrixXd> derivatives(N());

  for (int i = 0; i < num_modes_; i++) {
    for (int j = 0; j < mode_lengths_[i]; j++) {
      int k = mode_start_[i] + j;
      times_vec[k] = times(k);
      states[k] = GetSolution(state(k));
      inputs[k] = GetSolution(input(k));
      forces[k] = GetSolution(force(i, j));
      constraints_[i]->updateData(states[k], inputs[k], forces[k]);

    derivatives[k] = math::DiscardGradient(constraints_[i]->getXDot());//Do I need to make a copy here?
  }
}
  return PiecewisePolynomial<double>::Cubic(times_vec, states, derivatives);
}

template <typename T>
void HybridDircon<T>::SetInitialForceTrajectory(int mode, const PiecewisePolynomial<double>& traj_init_l,
                                                const PiecewisePolynomial<double>& traj_init_lc,
                                                const PiecewisePolynomial<double>& traj_init_vc) {
  double start_time = 0;
  double h;
  if (timesteps_are_decision_variables())
    h = GetInitialGuess(h_vars()[0]);
  else
    h = fixed_timestep();

  VectorXd guess_force(force_vars_[mode].size());
  if (traj_init_l.empty()) {
    guess_force.fill(0);  // Start with 0
  } else {
    for (int i = 0; i < N(); ++i) {
      guess_force.segment(num_kinematic_constraints_[mode] * i, num_kinematic_constraints_[mode]) =
          traj_init_l.value(start_time + i * h);
    }
  }
  SetInitialGuess(force_vars_[mode], guess_force);

  VectorXd guess_collocation_force(collocation_force_vars_[mode].size());
  if (traj_init_lc.empty()) {
    guess_collocation_force.fill(0);  // Start with 0
  } else {
    for (int i = 0; i < N()-1; ++i) {
      guess_collocation_force.segment(num_kinematic_constraints_[mode] * i, num_kinematic_constraints_[mode]) =
          traj_init_lc.value(start_time + (i + 0.5) * h);
    }
  }
  SetInitialGuess(collocation_force_vars_[mode], guess_collocation_force);

  VectorXd guess_collocation_slack(collocation_slack_vars_[mode].size());
  if (traj_init_vc.empty()) {
    guess_collocation_slack.fill(0);  // Start with 0
  } else {
    for (int i = 0; i < N()-1; ++i) {
      guess_collocation_slack.segment(num_kinematic_constraints_[mode] * i, num_kinematic_constraints_[mode]) =
          traj_init_vc.value(start_time + (i + 0.5) * h);
    }
  }
  SetInitialGuess(collocation_slack_vars_[mode], guess_collocation_slack); //call superclass method
}

template class HybridDircon<double>;
template class HybridDircon<AutoDiffXd>;

}  // namespace trajectory_optimization
}  // namespace systems
}  // namespace drake