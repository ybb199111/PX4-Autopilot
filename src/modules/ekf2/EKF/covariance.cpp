/****************************************************************************
 *
 *   Copyright (c) 2015-2023 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file covariance.cpp
 * Contains functions for initialising, predicting and updating the state
 * covariance matrix
 * equations generated using EKF/python/ekf_derivation/main.py
 *
 * @author Roman Bast <bastroman@gmail.com>
 *
 */

#include "ekf.h"
#include <ekf_derivation/generated/predict_covariance.h>

#include <math.h>
#include <mathlib/mathlib.h>

// Sets initial values for the covariance matrix
// Do not call before quaternion states have been initialised
void Ekf::initialiseCovariance()
{
	P.zero();

	resetQuatCov();

	// velocity
#if defined(CONFIG_EKF2_GNSS)
	const float vel_var = sq(fmaxf(_params.gps_vel_noise, 0.01f));
#else
	const float vel_var = sq(0.5f);
#endif
	P.uncorrelateCovarianceSetVariance<State::vel.dof>(State::vel.idx, Vector3f(vel_var, vel_var, sq(1.5f) * vel_var));

	// position
#if defined(CONFIG_EKF2_BAROMETER)
	float z_pos_var = sq(fmaxf(_params.baro_noise, 0.01f));
#else
	float z_pos_var = sq(1.f);
#endif // CONFIG_EKF2_BAROMETER

#if defined(CONFIG_EKF2_GNSS)
	const float xy_pos_var = sq(fmaxf(_params.gps_pos_noise, 0.01f));

	if (_control_status.flags.gps_hgt) {
		z_pos_var = sq(fmaxf(1.5f * _params.gps_pos_noise, 0.01f));
	}
#else
	const float xy_pos_var = sq(fmaxf(_params.pos_noaid_noise, 0.01f));
#endif

#if defined(CONFIG_EKF2_RANGE_FINDER)
	if (_control_status.flags.rng_hgt) {
		z_pos_var = sq(fmaxf(_params.range_noise, 0.01f));
	}
#endif // CONFIG_EKF2_RANGE_FINDER

	P.uncorrelateCovarianceSetVariance<State::pos.dof>(State::pos.idx, Vector3f(xy_pos_var, xy_pos_var, z_pos_var));

	resetGyroBiasCov();

	resetAccelBiasCov();

#if defined(CONFIG_EKF2_MAGNETOMETER)
	resetMagCov();
#endif // CONFIG_EKF2_MAGNETOMETER

#if defined(CONFIG_EKF2_WIND)
	resetWindCov();
#endif // CONFIG_EKF2_WIND
}

void Ekf::predictCovariance(const imuSample &imu_delayed)
{
	// Use average update interval to reduce accumulated covariance prediction errors due to small single frame dt values
	const float dt = _dt_ekf_avg;

	// delta angle noise variance
	float gyro_noise = math::constrain(_params.gyro_noise, 0.f, 1.f);
	const float gyro_var = sq(gyro_noise);

	// delta velocity noise variance
	float accel_noise = math::constrain(_params.accel_noise, 0.f, 1.f);
	Vector3f accel_var;

	for (unsigned i = 0; i < 3; i++) {
		if (_fault_status.flags.bad_acc_vertical || imu_delayed.delta_vel_clipping[i]) {
			// Increase accelerometer process noise if bad accel data is detected
			accel_var(i) = sq(BADACC_BIAS_PNOISE);

		} else {
			accel_var(i) = sq(accel_noise);
		}
	}

	// predict the covariance
	// calculate variances and upper diagonal covariances for quaternion, velocity, position and gyro bias states
	P = sym::PredictCovariance(_state.vector(), P,
		imu_delayed.delta_vel / math::max(imu_delayed.delta_vel_dt, FLT_EPSILON), accel_var,
		imu_delayed.delta_ang / math::max(imu_delayed.delta_ang_dt, FLT_EPSILON), gyro_var,
		0.5f * (imu_delayed.delta_vel_dt + imu_delayed.delta_ang_dt));

	// Construct the process noise variance diagonal for those states with a stationary process model
	// These are kinematic states and their error growth is controlled separately by the IMU noise variances

	// gyro bias: add process noise, or restore previous gyro bias var if state inhibited
	const float gyro_bias_sig = dt * math::constrain(_params.gyro_bias_p_noise, 0.f, 1.f);
	const float gyro_bias_process_noise = sq(gyro_bias_sig);
	for (unsigned index = 0; index < State::gyro_bias.dof; index++) {
		const unsigned i = State::gyro_bias.idx + index;

		if (!_gyro_bias_inhibit[index]) {
			P(i, i) += gyro_bias_process_noise;

		} else {
			P.uncorrelateCovarianceSetVariance<1>(i, _prev_gyro_bias_var(index));
		}
	}

	// accel bias: add process noise, or restore previous accel bias var if state inhibited
	const float accel_bias_sig = dt * math::constrain(_params.accel_bias_p_noise, 0.f, 1.f);
	const float accel_bias_process_noise = sq(accel_bias_sig);
	for (unsigned index = 0; index < State::accel_bias.dof; index++) {
		const unsigned i = State::accel_bias.idx + index;

		if (!_accel_bias_inhibit[index]) {
			P(i, i) += accel_bias_process_noise;

		} else {
			P.uncorrelateCovarianceSetVariance<1>(i, _prev_accel_bias_var(index));
		}
	}

#if defined(CONFIG_EKF2_MAGNETOMETER)
	if (_control_status.flags.mag) {
		// Don't continue to grow the earth field variances if they are becoming too large or we are not doing 3-axis fusion as this can make the covariance matrix badly conditioned
		if (P.trace<State::mag_I.dof>(State::mag_I.idx) < 0.1f) {

			float mag_I_sig = dt * math::constrain(_params.mage_p_noise, 0.f, 1.f);
			float mag_I_process_noise = sq(mag_I_sig);

			for (unsigned index = 0; index < State::mag_I.dof; index++) {
				unsigned i = State::mag_I.idx + index;
				P(i, i) += mag_I_process_noise;
			}
		}

		// Don't continue to grow the body field variances if they are becoming too large or we are not doing 3-axis fusion as this can make the covariance matrix badly conditioned
		if (P.trace<State::mag_B.dof>(State::mag_B.idx) < 0.1f) {

			float mag_B_sig = dt * math::constrain(_params.magb_p_noise, 0.f, 1.f);
			float mag_B_process_noise = sq(mag_B_sig);

			for (unsigned index = 0; index < State::mag_B.dof; index++) {
				unsigned i = State::mag_B.idx + index;
				P(i, i) += mag_B_process_noise;
			}
		}
	}
#endif // CONFIG_EKF2_MAGNETOMETER

#if defined(CONFIG_EKF2_WIND)
	if (_control_status.flags.wind) {
		// Don't continue to grow wind velocity state variances if they are becoming too large or we are not using wind velocity states as this can make the covariance matrix badly conditioned
		if (P.trace<State::wind_vel.dof>(State::wind_vel.idx) < sq(_params.initial_wind_uncertainty)) {

			float wind_vel_nsd_scaled = math::constrain(_params.wind_vel_nsd, 0.f, 1.f) * (1.f + _params.wind_vel_nsd_scaler * fabsf(_height_rate_lpf));

			const float wind_vel_process_noise = sq(wind_vel_nsd_scaled) * dt;

			for (unsigned index = 0; index < State::wind_vel.dof; index++) {
				unsigned i = State::wind_vel.idx + index;
				P(i, i) += wind_vel_process_noise;
			}
		}
	}
#endif // CONFIG_EKF2_WIND

	// covariance matrix is symmetrical, so copy upper half to lower half
	for (unsigned row = 0; row < State::size; row++) {
		for (unsigned column = 0 ; column < row; column++) {
			P(row, column) = P(column, row);
		}
	}

	// fix gross errors in the covariance matrix and ensure rows and
	// columns for un-used states are zero
	fixCovarianceErrors(false);
}

void Ekf::fixCovarianceErrors(bool force_symmetry)
{
	// NOTE: This limiting is a last resort and should not be relied on
	// TODO: Split covariance prediction into separate F*P*transpose(F) and Q contributions
	// and set corresponding entries in Q to zero when states exceed 50% of the limit
	// Covariance diagonal limits. Use same values for states which
	// belong to the same group (e.g. vel_x, vel_y, vel_z)
	const float quat_var_max = 1.0f;
	const float vel_var_max = 1e6f;
	const float pos_var_max = 1e6f;
	const float gyro_bias_var_max = 1.0f;

	constrainStateVar(State::quat_nominal, 0.f, quat_var_max);
	constrainStateVar(State::vel, 1e-6f, vel_var_max);
	constrainStateVar(State::pos, 1e-6f, pos_var_max);
	constrainStateVar(State::gyro_bias, 0.f, gyro_bias_var_max);

	// the following states are optional and are deactivated when not required
	// by ensuring the corresponding covariance matrix values are kept at zero

	// accelerometer bias states
	if (!_accel_bias_inhibit[0] || !_accel_bias_inhibit[1] || !_accel_bias_inhibit[2]) {
		// Find the maximum delta velocity bias state variance and request a covariance reset if any variance is below the safe minimum
		const float minSafeStateVar = 1e-9f / sq(_dt_ekf_avg);
		float maxStateVar = minSafeStateVar;
		bool resetRequired = false;

		for (unsigned axis = 0; axis < State::accel_bias.dof; axis++) {
			const unsigned stateIndex = State::accel_bias.idx + axis;

			if (_accel_bias_inhibit[axis]) {
				// Skip the check for the inhibited axis
				continue;
			}

			if (P(stateIndex, stateIndex) > maxStateVar) {
				maxStateVar = P(stateIndex, stateIndex);

			} else if (P(stateIndex, stateIndex) < minSafeStateVar) {
				resetRequired = true;
			}
		}

		// To ensure stability of the covariance matrix operations, the ratio of a max and min variance must
		// not exceed 100 and the minimum variance must not fall below the target minimum
		// Also limit variance to a maximum equivalent to a 0.1g uncertainty
		const float minStateVarTarget = 5E-8f / sq(_dt_ekf_avg);
		float minAllowedStateVar = fmaxf(0.01f * maxStateVar, minStateVarTarget);

		for (unsigned axis = 0; axis < State::accel_bias.dof; axis++) {
			const unsigned stateIndex = State::accel_bias.idx + axis;

			if (_accel_bias_inhibit[axis]) {
				// Skip the check for the inhibited axis
				continue;
			}

			P(stateIndex, stateIndex) = math::constrain(P(stateIndex, stateIndex), minAllowedStateVar, sq(0.1f * CONSTANTS_ONE_G));
		}

		// If any one axis has fallen below the safe minimum, all delta velocity covariance terms must be reset to zero
		if (resetRequired) {
			resetAccelBiasCov();
		}
	}

	if (force_symmetry) {
		P.makeRowColSymmetric<State::quat_nominal.dof>(State::quat_nominal.idx);
		P.makeRowColSymmetric<State::vel.dof>(State::vel.idx);
		P.makeRowColSymmetric<State::pos.dof>(State::pos.idx);
		P.makeRowColSymmetric<State::gyro_bias.dof>(State::gyro_bias.idx);
		P.makeRowColSymmetric<State::accel_bias.dof>(State::accel_bias.idx);
	}

#if defined(CONFIG_EKF2_MAGNETOMETER)
	// magnetic field states
	if (!_control_status.flags.mag) {
		P.uncorrelateCovarianceSetVariance<State::mag_I.dof>(State::mag_I.idx, 0.0f);
		P.uncorrelateCovarianceSetVariance<State::mag_B.dof>(State::mag_B.idx, 0.0f);

	} else {
		const float mag_I_var_max = 1.f;
		constrainStateVar(State::mag_I, 0.f, mag_I_var_max);

		const float mag_B_var_max = 1.f;
		constrainStateVar(State::mag_B, 0.f, mag_B_var_max);

		if (force_symmetry) {
			P.makeRowColSymmetric<State::mag_I.dof>(State::mag_I.idx);
			P.makeRowColSymmetric<State::mag_B.dof>(State::mag_B.idx);
		}
	}
#endif // CONFIG_EKF2_MAGNETOMETER

#if defined(CONFIG_EKF2_WIND)
	// wind velocity states
	if (!_control_status.flags.wind) {
		P.uncorrelateCovarianceSetVariance<State::wind_vel.dof>(State::wind_vel.idx, 0.0f);

	} else {
		const float wind_vel_var_max = 1e6f;
		constrainStateVar(State::wind_vel, 0.f, wind_vel_var_max);

		if (force_symmetry) {
			P.makeRowColSymmetric<State::wind_vel.dof>(State::wind_vel.idx);
		}
	}
#endif // CONFIG_EKF2_WIND
}

void Ekf::constrainStateVar(const IdxDof &state, float min, float max)
{
	for (unsigned i = state.idx; i < (state.idx + state.dof); i++) {
		P(i, i) = math::constrain(P(i, i), min, max);
	}
}

// if the covariance correction will result in a negative variance, then
// the covariance matrix is unhealthy and must be corrected
bool Ekf::checkAndFixCovarianceUpdate(const SquareMatrixState &KHP)
{
	bool healthy = true;

	for (int i = 0; i < State::size; i++) {
		if (P(i, i) < KHP(i, i)) {
			P.uncorrelateCovarianceSetVariance<1>(i, 0.0f);
			healthy = false;
		}
	}

	return healthy;
}

void Ekf::resetQuatCov(const float yaw_noise)
{
	const float tilt_var = sq(math::max(_params.initial_tilt_err, 0.01f));
	float yaw_var = sq(0.01f);

	// update the yaw angle variance using the variance of the measurement
	if (PX4_ISFINITE(yaw_noise)) {
		// using magnetic heading tuning parameter
		yaw_var = math::max(sq(yaw_noise), yaw_var);
	}

	resetQuatCov(Vector3f(tilt_var, tilt_var, yaw_var));
}

void Ekf::resetQuatCov(const Vector3f &rot_var_ned)
{
	matrix::SquareMatrix<float, State::quat_nominal.dof> q_cov_ned = diag(rot_var_ned);
	resetStateCovariance<State::quat_nominal>(_R_to_earth.T() * q_cov_ned * _R_to_earth);
}

#if defined(CONFIG_EKF2_MAGNETOMETER)
void Ekf::resetMagCov()
{
	if (_mag_decl_cov_reset) {
		ECL_INFO("reset mag covariance");
		_mag_decl_cov_reset = false;
	}

	P.uncorrelateCovarianceSetVariance<State::mag_I.dof>(State::mag_I.idx, sq(_params.mag_noise));
	P.uncorrelateCovarianceSetVariance<State::mag_B.dof>(State::mag_B.idx, sq(_params.mag_noise));

	saveMagCovData();
}
#endif // CONFIG_EKF2_MAGNETOMETER

void Ekf::resetGyroBiasZCov()
{
	const float init_gyro_bias_var = sq(_params.switch_on_gyro_bias);

	P.uncorrelateCovarianceSetVariance<1>(State::gyro_bias.idx + 2, init_gyro_bias_var);
}
