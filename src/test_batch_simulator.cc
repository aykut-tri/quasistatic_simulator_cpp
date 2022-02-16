#include <iostream>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "batch_quasistatic_simulator.h"
#include "get_model_paths.h"

using drake::multibody::ModelInstanceIndex;
using Eigen::MatrixXd;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;
using std::cout;
using std::endl;
using std::string;

MatrixXd CreateRandomMatrix(int n_rows, int n_cols, std::mt19937 &gen) {
  std::uniform_real_distribution<double> dis(-1.0, 1.0);
  return MatrixXd::NullaryExpr(n_rows, n_cols, [&]() { return dis(gen); });
}

class TestBatchQuasistaticSimulator : public ::testing::Test {
protected:
  void SetUp() override {
    // Make sure that n_tasks_ is not divisible by hardware_concurrency.
    n_tasks_ = std::thread::hardware_concurrency() * 20 + 1;
  }

  // TODO: simplify these setup functions with QuasistaticParser.
  void SetUpPlanarHand() {
    const string kObjectSdfPath =
        GetQsimModelsPath() / "sphere_yz_rotation_r_0.25m.sdf";

    const string kModelDirectivePath = GetQsimModelsPath() / "planar_hand.yml";

    QuasistaticSimParameters sim_params;
    sim_params.gravity = Vector3d(0, 0, -10);
    sim_params.nd_per_contact = 2;
    sim_params.contact_detection_tolerance = 1.0;
    sim_params.is_quasi_dynamic = true;
    sim_params.gradient_from_active_constraints = true;

    VectorXd Kp(2);
    Kp << 50, 25;
    const string robot_l_name = "arm_left";
    const string robot_r_name = "arm_right";

    std::unordered_map<string, VectorXd> robot_stiffness_dict = {
        {robot_l_name, Kp}, {robot_r_name, Kp}};

    const string object_name("sphere");
    std::unordered_map<string, string> object_sdf_dict = {
        {object_name, kObjectSdfPath}};

    q_sim_batch_ = std::make_unique<BatchQuasistaticSimulator>(
        kModelDirectivePath, robot_stiffness_dict, object_sdf_dict, sim_params);

    auto &q_sim = q_sim_batch_->get_q_sim();
    const auto name_to_idx_map = q_sim.GetModelInstanceNameToIndexMap();
    const auto idx_l = name_to_idx_map.at(robot_l_name);
    const auto idx_r = name_to_idx_map.at(robot_r_name);
    const auto idx_o = name_to_idx_map.at(object_name);

    ModelInstanceIndexToVecMap q0_dict = {{idx_o, Vector3d(0, 0.316, 0)},
                                          {idx_l, Vector2d(-0.775, -0.785)},
                                          {idx_r, Vector2d(0.775, 0.785)}};

    VectorXd q0 = q_sim.GetQVecFromDict(q0_dict);
    VectorXd u0 = q_sim.GetQaCmdVecFromDict(q0_dict);

    SampleUBatch(u0, 0.1);
    SetXBatch(q0);
  }

  void SetUpAllegroHand() {
    const string kObjectSdfPath = GetQsimModelsPath() / "sphere_r0.06m.sdf";
    const string kModelDirectivePath = GetQsimModelsPath() / "allegro_hand.yml";

    QuasistaticSimParameters sim_params;
    sim_params.gravity = Vector3d(0, 0, 0);
    sim_params.nd_per_contact = 4;
    sim_params.contact_detection_tolerance = 0.025;
    sim_params.is_quasi_dynamic = true;
    sim_params.gradient_from_active_constraints = true;

    constexpr int n_qa = 16;
    VectorXd Kp = VectorXd::Ones(n_qa) * 100;
    const string robot_name("allegro_hand_right");

    std::unordered_map<string, VectorXd> robot_stiffness_dict = {
        {robot_name, Kp}};

    const string object_name("sphere");
    std::unordered_map<string, string> object_sdf_dict = {
        {object_name, kObjectSdfPath}};

    q_sim_batch_ = std::make_unique<BatchQuasistaticSimulator>(
        kModelDirectivePath, robot_stiffness_dict, object_sdf_dict, sim_params);

    auto &q_sim = q_sim_batch_->get_q_sim();
    const auto name_to_idx_map = q_sim.GetModelInstanceNameToIndexMap();
    const auto idx_r = name_to_idx_map.at(robot_name);
    const auto idx_o = name_to_idx_map.at(object_name);

    VectorXd q_u0(7);
    q_u0 << 0.96040786, 0.07943188, 0.26694634, 0.00685272, -0.08083068,
        0.00117524, 0.0711;

    VectorXd q_a0(n_qa);
    q_a0 << 0.03501504, 0.75276565, 0.74146232, 0.83261002, 0.63256269,
        1.02378254, 0.64089555, 0.82444782, -0.1438725, 0.74696812, 0.61908827,
        0.70064279, -0.06922541, 0.78533142, 0.82942863, 0.90415436;

    ModelInstanceIndexToVecMap q0_dict = {{idx_o, q_u0}, {idx_r, q_a0}};

    VectorXd q0 = q_sim.GetQVecFromDict(q0_dict);
    VectorXd u0 = q_sim.GetQaCmdVecFromDict(q0_dict);

    SampleUBatch(u0, 0.1);
    SetXBatch(q0);
  };

  void SampleUBatch(const Eigen::Ref<const Eigen::VectorXd> &u0,
                    double interval_size) {
    std::mt19937 gen(1);
    u_batch_ = interval_size * CreateRandomMatrix(n_tasks_, u0.size(), gen);
    u_batch_.rowwise() += u0.transpose();
  }

  void SetXBatch(const Eigen::Ref<const Eigen::VectorXd> &x0) {
    x_batch_.resize(n_tasks_, x0.size());
    x_batch_.setZero();
    x_batch_.rowwise() += x0.transpose();
  }

  void CompareIsValid(const std::vector<bool> &is_valid_batch_1,
                      const std::vector<bool> &is_valid_batch_2) const {
    EXPECT_EQ(n_tasks_, is_valid_batch_1.size());
    EXPECT_EQ(n_tasks_, is_valid_batch_2.size());
    for (int i = 0; i < is_valid_batch_1.size(); i++) {
      EXPECT_EQ(is_valid_batch_1[i], is_valid_batch_2[i]);
    }
  }

  void CompareXNext(const Eigen::Ref<const MatrixXd> &x_next_batch_1,
                    const Eigen::Ref<const MatrixXd> &x_next_batch_2) const {
    EXPECT_EQ(n_tasks_, x_next_batch_1.rows());
    EXPECT_EQ(n_tasks_, x_next_batch_2.rows());
    const double avg_diff =
        (x_next_batch_2 - x_next_batch_1).matrix().rowwise().norm().sum() /
        n_tasks_;
    EXPECT_LT(avg_diff, 1e-6);
  }

  void CompareB(const std::vector<MatrixXd> &B_batch_1,
                const std::vector<MatrixXd> &B_batch_2,
                const double tol) const {
    EXPECT_EQ(n_tasks_, B_batch_1.size());
    EXPECT_EQ(n_tasks_, B_batch_2.size());
    for (int i = 0; i < n_tasks_; i++) {
      double err = (B_batch_1[i] - B_batch_2[i]).norm();
      double rel_err = err / B_batch_1[i].norm();
      EXPECT_LT(err, tol);
      EXPECT_LT(rel_err, 0.01);
    }
  }

  int n_tasks_{0};
  double h_{0.1};
  MatrixXd u_batch_, x_batch_;
  std::unique_ptr<BatchQuasistaticSimulator> q_sim_batch_;
};

TEST_F(TestBatchQuasistaticSimulator, TestForwardDynamicsPlanarHand) {
  SetUpPlanarHand();
  auto [x_next_batch_parallel, B_batch_parallel, is_valid_batch_parallel] =
      q_sim_batch_->CalcDynamicsParallel(
          x_batch_, u_batch_, h_, GradientMode::kNone, {});

  auto [x_next_batch_serial, B_batch_serial, is_valid_batch_serial] =
      q_sim_batch_->CalcDynamicsSerial(
          x_batch_, u_batch_, h_, GradientMode::kNone, {});
  // is_valid.
  CompareIsValid(is_valid_batch_parallel, is_valid_batch_serial);

  // x_next.
  CompareXNext(x_next_batch_parallel, x_next_batch_serial);

  // B.
  EXPECT_EQ(B_batch_parallel.size(), 0);
  EXPECT_EQ(B_batch_serial.size(), 0);
}

TEST_F(TestBatchQuasistaticSimulator, TestForwardDynamicsAllegroHand) {
  SetUpAllegroHand();
  auto [x_next_batch_parallel, B_batch_parallel, is_valid_batch_parallel] =
      q_sim_batch_->CalcDynamicsParallel(
          x_batch_, u_batch_, h_, GradientMode::kNone, {});

  auto [x_next_batch_serial, B_batch_serial, is_valid_batch_serial] =
      q_sim_batch_->CalcDynamicsSerial(
          x_batch_, u_batch_, h_, GradientMode::kNone, {});
  // is_valid.
  CompareIsValid(is_valid_batch_parallel, is_valid_batch_serial);

  // x_next.
  CompareXNext(x_next_batch_parallel, x_next_batch_serial);

  // B.
  EXPECT_EQ(B_batch_parallel.size(), 0);
  EXPECT_EQ(B_batch_serial.size(), 0);
}

TEST_F(TestBatchQuasistaticSimulator, TestGradientPlanarHand) {
  SetUpPlanarHand();
  auto [x_next_batch_parallel, B_batch_parallel, is_valid_batch_parallel] =
      q_sim_batch_->CalcDynamicsParallel(
          x_batch_, u_batch_, h_, GradientMode::kBOnly, {});

  auto [x_next_batch_serial, B_batch_serial, is_valid_batch_serial] =
      q_sim_batch_->CalcDynamicsSerial(
          x_batch_, u_batch_, h_, GradientMode::kBOnly, {});

  // is_valid.
  CompareIsValid(is_valid_batch_parallel, is_valid_batch_serial);

  // x_next.
  CompareXNext(x_next_batch_parallel, x_next_batch_serial);

  // B.
  CompareB(B_batch_parallel, B_batch_serial, 1e-6);
}

TEST_F(TestBatchQuasistaticSimulator, TestGradientAllegroHand) {
  SetUpAllegroHand();
  auto [x_next_batch_parallel, B_batch_parallel, is_valid_batch_parallel] =
      q_sim_batch_->CalcDynamicsParallel(
          x_batch_, u_batch_, h_, GradientMode::kBOnly, {});

  auto [x_next_batch_serial, B_batch_serial, is_valid_batch_serial] =
      q_sim_batch_->CalcDynamicsSerial(
          x_batch_, u_batch_, h_, GradientMode::kBOnly, {});

  // is_valid.
  CompareIsValid(is_valid_batch_parallel, is_valid_batch_serial);

  // x_next.
  CompareXNext(x_next_batch_parallel, x_next_batch_serial);

  // B.
  CompareB(B_batch_parallel, B_batch_serial, 2e-6);
}

/*
 * Compare BatchQuasistaticSimulator::CalcBundledBTrjDirect against
 *        BatchQuasistaticSimulator::CalcBundledBTrj.
 * The goal is to ensure that the outcomes of these two functions are the
 * same given the same seed for the random number generator.
 */
TEST_F(TestBatchQuasistaticSimulator, TestBundledB) {
  SetUpPlanarHand();
  const int T = 50;
  const int n_samples = 100;
  const int seed = 1;

  const int n_q = q_sim_batch_->get_q_sim().get_plant().num_positions();
  const int n_u = q_sim_batch_->get_q_sim().num_actuated_dofs();
  ASSERT_EQ(n_q, x_batch_.cols());
  ASSERT_EQ(n_u, u_batch_.cols());

  MatrixXd x_trj(T + 1, n_q);
  MatrixXd u_trj(T, n_u);

  x_trj.rowwise() = x_batch_.row(0);
  u_trj.rowwise() = u_batch_.row(0);

  auto B_bundled1 =
      q_sim_batch_->CalcBundledBTrj(x_trj, u_trj, 0.1, 0.1, n_samples, seed);
  auto B_bundled2 = q_sim_batch_->CalcBundledBTrjDirect(x_trj, u_trj, 0.1, 0.1,
                                                        n_samples, seed);
  for (int i = 0; i < T; i++) {
    double err = (B_bundled1[i] - B_bundled2[i]).norm();
    EXPECT_LT(err, 1e-10);
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
