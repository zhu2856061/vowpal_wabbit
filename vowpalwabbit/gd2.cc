/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
*/
#include "vw_base.h"

#include "crossplat_compat.h"

#include <float.h>
#ifdef _WIN32
#include <WinSock2.h>
#else
#include <netdb.h>
#endif

#if !defined(VW_NO_INLINE_SIMD)
#  if defined(__ARM_NEON__)
#include <arm_neon.h>
#  elif defined(__SSE2__)
#include <xmmintrin.h>
#  endif
#endif

#include "gd.h"
#include "accumulate.h"
#include "reductions.h"
#include "vw.h"
#include "floatbits.h"

#define VERSION_SAVE_RESUME_FIX "7.10.1"

using namespace std;
using namespace LEARNER;

inline void vec_add(float& p, const float fx, float& fw) { p += fw * fx; }

inline float sign(float w) { if (w < 0.) return -1.; else  return 1.; }

inline float trunc_weight(const float w, const float gravity)
{
	return (gravity < fabsf(w)) ? w - sign(w) * gravity : 0.f;
}

#include <algorithm>

struct string_value
{
	float v;
	string s;
	friend bool operator<(const string_value& first, const string_value& second);
};

bool operator<(const string_value& first, const string_value& second)
{
	return fabsf(first.v) > fabsf(second.v);
}

struct multipredict_info { size_t count; size_t step; float* pred; weight_parameters& weights; /* & for l1: */ float gravity; };

struct audit_results
{
	vw& all;
	const uint64_t offset;
	vector<string> ns_pre;
	vector<string_value> results;
	audit_results(vw& p_all, const size_t p_offset) :all(p_all), offset(p_offset) {}
};


inline void audit_interaction(audit_results& dat, const audit_strings* f)
{
	if (f == nullptr)
	{
		if (!dat.ns_pre.empty())
		{
			dat.ns_pre.pop_back();
		}

		return;
	}

	string ns_pre;
	if (!dat.ns_pre.empty())
		ns_pre += '*';

	if (f->first != "" && ((f->first) != " "))
	{
		ns_pre.append(f->first);
		ns_pre += '^';
	}

	if (f->second != "")
	{
		ns_pre.append(f->second);
	}

	if (!ns_pre.empty())
	{
		dat.ns_pre.push_back(ns_pre);
	}
}

inline void audit_feature(audit_results& dat, const float ft_weight, const uint64_t ft_idx)
{
	weight_parameters& weights = dat.all.weights;
	uint64_t index = ft_idx & weights.mask();
	size_t stride_shift = dat.all.weights.stride_shift();

	string ns_pre;
	for (string& s : dat.ns_pre) ns_pre += s;

	if (dat.all.audit)
	{
		ostringstream tempstream;
		tempstream << ':' << (index >> stride_shift) << ':' << ft_weight
			<< ':' << trunc_weight(weights[index], (float)dat.all.sd->gravity) * (float)dat.all.sd->contraction;

		if (dat.all.adaptive)
			tempstream << '@' << weights[index + 1];


		string_value sv = { weights[index] * ft_weight, ns_pre + tempstream.str() };
		dat.results.push_back(sv);
	}

	if (dat.all.current_pass == 0 && dat.all.hash_inv)
	{ //for invert_hash

		if (dat.offset != 0)
		{ // otherwise --oaa output no features for class > 0.
			ostringstream tempstream;
			tempstream << '[' << (dat.offset >> stride_shift) << ']';
			ns_pre += tempstream.str();
		}

		if (!dat.all.name_index_map.count(ns_pre))
			dat.all.name_index_map.insert(std::map< std::string, size_t>::value_type(ns_pre, index >> stride_shift));
	}

}

class GDArgs : public IArguments
{

public:
	float sparse_l2 = 0.;
	bool sgd = false;
	bool adaptive = false;
	bool invariant = false;
	bool normalized = false;
	//double normalized_sum_norm_x;
	double total_weight = 0;
	size_t no_win_counter = 0;
	size_t early_stop_thres = 3;
	float initial_constant = 0.;
	float neg_norm_power = 0.;
	float neg_power_t = 0.;
	float update_multiplier = 0.;
	bool l1 = false;
	bool audit = false;
	bool feature_mask_off = true;
	bool sqrt_rate;
	bool sparse_l2_t;
	int reg_mode;
	bool hash_inv;

	po::options_description* options()
	{
		auto opts = new po::options_description();
		opts->add_options()
			("sgd", po::value<decltype(sgd)>(&sgd), "use regular stochastic gradient descent update.")
			("adaptive", po::value<decltype(adaptive)>(&adaptive), "use adaptive, individual learning rates.")
			("invariant", po::value<decltype(invariant)>(&invariant)->default_value(false), "use safe/importance aware updates.")
			("normalized", po::value<decltype(normalized)>(&normalized), "use per feature normalized updates")
			("sparse_l2", po::value<decltype(sparse_l2)>(&sparse_l2)->default_value(0.f), "use per feature normalized updates");

		return opts;
	}

	Learner* create(vw& all);
};

class GDLearner : public LearnerBase<GDLearner, GDArgs, float, float>
{
	typedef LearnerBase<GDLearner, GDArgs, float, float> TLearnerBase;
public:
	typedef float TPrediction;
	typedef float TLabel;



	template<bool is_learn>
	struct PredictOrLearnFactory
	{
		template<bool l1, bool audit, bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, bool adaptive, bool normalized>
		static PredictOrLearnMethod create()
		{
			const int adapative_v = adaptive ? 1 : 2;
			const int normalized_v = normalized ? adapative_v : 0;
			const int spare = adaptive ? 0 : 2;
			return 
				static_cast<PredictOrLearnMethod>(&predict_or_learn_dispatch<GDLearner, TPrediction, TLabel,
					&GDLearner::template predict_or_learn<l1, audit, sparse_l2, invariant, sqrt_rate, feature_mask_off, adapative_v, normalized_v, spare>>);
		}
	};

	struct UpdateFactory
	{
		template<bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, bool adaptive, bool normalized>
		static UpdateMethod create()
		{
			const int adapative_v = adaptive ? 1 : 2;
			const int normalized_v = normalized ? adapative_v : 0;
			const int spare = adaptive ? 0 : 2;
			return 
				&update_dispatch<GDLearner, TPrediction, TLabel,
					&GDLearner::template update<sparse_l2, invariant, sqrt_rate, feature_mask_off, adapative_v, normalized_v, spare>>;
		}
	};

	struct MultiPredictFactory
	{
		template<bool l1, bool audit>
		static MultiPredictMethod create()
		{
			return &multi_predict_dispatch<GDLearner, TPrediction, TLabel, &GDLearner::template multi_predict<l1, audit>>;
		}
	};

	struct SensitivityFactory
	{
		template<bool adaptive, bool normalized, bool sqrt_rate, bool feature_mask_off>
		static SensitivityMethod create()
		{
			const int adapative_v = adaptive ? 1 : 2;
			const int normalized_v = normalized ? adapative_v : 0;
			const int spare = adaptive ? 0 : 2;
			return 
				&sensitivity_dispatch<GDLearner, TPrediction, TLabel,
					&GDLearner::sensitivity<sqrt_rate, feature_mask_off, adapative_v, normalized_v, spare>>;
		}
	};

	static TypedLearnerVTable<TPrediction, TLabel> GetVTable(GDArgs& args)
	{
		const bool l1 = args.reg_mode % 2 == 1;
		const bool audit = args.audit || args.hash_inv;

		TypedLearnerVTable<TPrediction, TLabel> vtable;

		vtable.learn_method = template_expansion<PredictOrLearnMethod, PredictOrLearnFactory<true>>::template expand(
			l1, audit, args.sparse_l2, args.invariant, args.sqrt_rate, args.feature_mask_off, args.adaptive, args.normalized);

		vtable.predict_method = template_expansion<PredictOrLearnMethod, PredictOrLearnFactory<false>>::template expand(
			l1, audit, args.sparse_l2, args.invariant, args.sqrt_rate, args.feature_mask_off, args.adaptive, args.normalized);

		vtable.update_method = template_expansion<UpdateMethod, UpdateFactory>::template expand(
			args.sparse_l2, args.invariant, args.sqrt_rate, args.feature_mask_off, args.adaptive, args.normalized);

		vtable.multi_predict_method = template_expansion<MultiPredictMethod, MultiPredictFactory>::template expand(l1, audit);
		vtable.sensitivity_method = template_expansion<SensitivityMethod, SensitivityFactory>::template expand(args.adaptive, args.normalized, args.sqrt_rate, args.feature_mask_off);

		return vtable;
	}

public:
private:
	vw& all; // TODO remove me

public:
	GDLearner(GDArgs& args, vw& _all) :
		LearnerBase<GDLearner, GDArgs, float, float>(args, _all, GetVTable(args)),
		all(_all)
	{
		_all.normalized_sum_norm_x = 0;
	}



private:
	inline float quake_InvSqrt(float x)
	{ // Carmack/Quake/SGI fast method:
		float xhalf = 0.5f * x;
		int i = float_to_bits(x); // store floating-point bits in integer
		i = 0x5f3759d5 - (i >> 1); // initial guess for Newton's method
		x = bits_to_float(i); // convert new bits into float
		x = x*(1.5f - xhalf*x*x); // One round of Newton's method
		return x;
	}

	static inline float InvSqrt(float x)
	{
#if !defined(VW_NO_INLINE_SIMD)
#  if defined(__ARM_NEON__)
		// Propagate into vector
		float32x2_t v1 = vdup_n_f32(x);
		// Estimate
		float32x2_t e1 = vrsqrte_f32(v1);
		// N-R iteration 1
		float32x2_t e2 = vmul_f32(e1, vrsqrts_f32(v1, vmul_f32(e1, e1)));
		// N-R iteration 2
		float32x2_t e3 = vmul_f32(e2, vrsqrts_f32(v1, vmul_f32(e2, e2)));
		// Extract result
		return vget_lane_f32(e3, 0);
#  elif (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64))
		__m128 eta = _mm_load_ss(&x);
		eta = _mm_rsqrt_ss(eta);
		_mm_store_ss(&x, eta);
#else
		x = quake_InvSqrt(x);
#  endif
#else
		x = quake_InvSqrt(x);
#endif

		return x;
	}

	//template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	template<bool feature_mask_off, size_t spare>
	inline void update_feature(float& update, float x, float& fw)
	{
		weight* w = &fw;
		if (feature_mask_off || fw != 0.)
		{
			if (spare != 0)
				x *= w[spare];
			w[0] += update * x;
		}
	}

	//this deals with few nonzero features vs. all nonzero features issues.
	template<bool sqrt_rate, size_t adaptive, size_t normalized>
	float average_update()
	{
		if (normalized)
		{
			if (sqrt_rate)
			{
				float avg_norm = (float)_args.total_weight / (float)_all.normalized_sum_norm_x;
				if (adaptive)
					return sqrt(avg_norm);
				else
					return avg_norm;
			}
			else
				return powf((float)_all.normalized_sum_norm_x / (float)_args.total_weight, _args.neg_norm_power);
		}
		return 1.f;
	}

	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	void train(example& ec, float update)
	{
		if (normalized)
			update *= _args.update_multiplier;
		// TODO foreach_feature<decltype(update), update_feature>(ec, update);
	}

	//void end_pass()
	//{
	//	vw& all = *g.all;
	//	sync_weights(all);
	//	if (all.all_reduce != nullptr)
	//	{
	//		if (all.adaptive)
	//			accumulate_weighted_avg(all, all.weights);
	//		else
	//			accumulate_avg(all, all.weights, 0);
	//	}
	//	all.eta *= all.eta_decay_rate;
	//	if (all.save_per_pass)
	//		save_predictor(all, all.final_regressor_name, all.current_pass);

	//	all.current_pass++;

	//	if (!all.holdout_set_off)
	//	{
	//		if (summarize_holdout_set(all, g.no_win_counter))
	//			finalize_regressor(all, all.final_regressor_name);
	//		if ((g.early_stop_thres == g.no_win_counter) &&
	//			((all.check_holdout_every_n_passes <= 1) ||
	//			((all.current_pass % all.check_holdout_every_n_passes) == 0)))
	//			set_done(all);
	//	}
	//}


	

	void print_features(example& ec)
	{
		weight_parameters& weights = _all.weights;

		if (_all.lda > 0)
		{
			size_t count = 0;
			for (features& fs : ec)
				count += fs.size();
			for (features& fs : ec)
			{
				for (features::iterator_all& f : fs.values_indices_audit())
				{
					cout << '\t' << f.audit().get()->first << '^' << f.audit().get()->second << ':' << ((f.index() >> _all.weights.stride_shift()) & _all.parse_mask) << ':' << f.value();
					for (size_t k = 0; k < _all.lda; k++)
						cout << ':' << weights[(f.index() + k)];
				}
			}
			cout << " total of " << count << " features." << endl;
		}
		else
		{

			audit_results dat(_all, ec.ft_offset);

			for (features& fs : ec)
			{
				if (fs.space_names.size() > 0)
					for (features::iterator_all& f : fs.values_indices_audit())
					{
						audit_interaction(dat, f.audit().get());
						audit_feature(dat, f.value(), f.index() + ec.ft_offset);
						audit_interaction(dat, NULL);
					}
				else
					for (features::iterator& f : fs)
						audit_feature(dat, f.value(), f.index() + ec.ft_offset);
			}

			INTERACTIONS::generate_interactions<audit_results, const uint64_t, audit_feature, true, audit_interaction >(_all, ec, dat);

			sort(dat.results.begin(), dat.results.end());
			if (_all.audit)
			{
				for (string_value& sv : dat.results)
					cout << '\t' << sv.s;
				cout << endl;
			}
		}
	}

	void print_audit_features(example& ec)
	{
		if (_all.audit)
			print_result(all.stdout_fileno, ec.pred.scalar, -1, ec.tag);
		fflush(stdout);
		print_features(ec);
	}

	float finalize_prediction(float ret)
	{
		shared_data* sd = _all.sd;
		if (nanpattern(ret))
		{
			float ret = 0.;
			if (ret > sd->max_label) ret = (float)sd->max_label;
			if (ret < sd->min_label) ret = (float)sd->min_label;
			cerr << "NAN prediction in example " << sd->example_number + 1 << ", forcing " << ret << endl;
			return ret;
		}
		if (ret > sd->max_label)
			return (float)sd->max_label;
		if (ret < sd->min_label)
			return (float)sd->min_label;
		return ret;
	}

	struct trunc_data
	{
		float prediction;
		float gravity;
	};

	inline void vec_add_trunc(const float fx, float& fw, float gravity, float* prediction)
	{
		*prediction += trunc_weight(fw, gravity) * fx;
	}

	inline float trunc_predict(example& ec, double gravity)
	{
		// trunc_data temp = { ec.l.simple.initial, (float)gravity };
		float prediction = ec.l.simple.initial;
		//foreach_feature<trunc_data, vec_add_trunc>(ec, temp);
		// ff<float, float&>::foreach_feature<&GDLearnerBound::vec_add_trunc>(*this, ec, (float)gravity, prediction);
		//foreach_feature_m(GDLearnerBound::vec_add_trunc, ec, (float)gravity, &prediction);
		foreach_feature_m(GDLearner::vec_add_trunc, ec, (float)gravity, &prediction);

		return prediction;
	}

	inline void vec_add_print(const float fx, float& fw, float* p)
	{
		*p += fw * fx;
		cerr << " + " << fw << "*" << fx;
	}
	
	inline float inline_predict(example& ec)
	{
		float temp = ec.l.simple.initial;
		//foreach_feature_m(GDLearnerBound::vec_add_print, ec, &temp);
		foreach_feature_m(GDLearner::vec_add_print, ec, &temp);

		// FINAL foreach_feature<float&>().values<&GDLearnerBound::vec_add_print>(temp);

		// foreach_feature<float, vec_add>(_all, ec, temp);
		// ff<float&>::foreach_feature<&GDLearnerBound::vec_add_print>(*this, ec, temp);
		return temp;
	}

	//template<bool l1, bool audit>
	//void predict(gd& g, base_learner&, example& ec)
	//{
	//	vw& all = *g.all;
	//	if (l1)
	//		ec.partial_prediction = trunc_predict(all, ec, all.sd->gravity);
	//	else
	//		ec.partial_prediction = inline_predict(all, ec);

	//	ec.partial_prediction *= (float)all.sd->contraction;
	//	ec.pred.scalar = finalize_prediction(all.sd, ec.partial_prediction);
	//	if (audit)
	//		print_audit_features(all, ec);
	//}

	inline void vec_add_trunc_multipredict(multipredict_info& mp, const float fx, uint64_t fi)
	{
		weight_parameters w = mp.weights;
		size_t index = fi;
		for (size_t c = 0; c<mp.count; c++, index += mp.step)
		{
			mp.pred[c] += fx * trunc_weight(w[index], mp.gravity); //TODO: figure out how to use weight_parameters::iterator (not change_begin)
		}
	}

	void sync_weights(vw& all)
	{
		if (all.sd->gravity == 0. && all.sd->contraction == 1.)  // to avoid unnecessary weight synchronization
			return;
		weight_parameters& weights = all.weights;
		weight_parameters::iterator w = weights.begin();
		for (; w != weights.end() && all.reg_mode; ++w)
			*w = trunc_weight(*w, (float)all.sd->gravity) * (float)all.sd->contraction;
		all.sd->gravity = 0.;
		all.sd->contraction = 1.;
	}

public: 
	// TODO: void multi_predict(example& ec, size_t lo, size_t count, float* pred, float& label, bool finalize_predictions)
	template<bool l1, bool audit>
	void multi_predict(example& ec, size_t count, size_t step, float* pred, float& label, bool finalize_predictions)
	//template<bool l1, bool audit>
	//void multipredict(gd& g, base_learner&, example& ec, size_t count, size_t step, polyprediction*pred, bool finalize_predictions)
	{
		for (size_t c = 0; c<count; c++)
			pred[c] = ec.l.simple.initial;
			//pred[c].scalar = ec.l.simple.initial;
		multipredict_info mp = { count, step, pred, _all.weights, (float)_all.sd->gravity };
		// TODO
		//if (l1) foreach_feature<multipredict_info, uint64_t, vec_add_trunc_multipredict>(ec, mp);
		//else    foreach_feature<multipredict_info, uint64_t, vec_add_multipredict      >(ec, mp);
		if (_all.sd->contraction != 1.)
			for (size_t c = 0; c<count; c++)
				pred[c] *= (float)_all.sd->contraction;
				//pred[c].scalar *= (float)_all.sd->contraction;
		if (finalize_predictions)
			for (size_t c = 0; c<count; c++)
				pred[c] = finalize_prediction(pred[c]);
				//pred[c].scalar = finalize_prediction(pred[c].scalar);
		if (audit)
		{
			RESTORE_VALUE_ON_RETURN(ec.ft_offset);
			for (size_t c = 0; c<count; c++)
			{
				ec.pred.scalar = pred[c];
				// ec.pred.scalar = pred[c].scalar;
				print_audit_features(ec);
				ec.ft_offset += (uint64_t)step;
			}
			//ec.ft_offset -= (uint64_t)(step*count);
		}
	}
	private:

	struct power_data
	{
		float minus_power_t;
		float neg_norm_power;
	};

	template<bool sqrt_rate, size_t adaptive, size_t normalized>
	inline float compute_rate_decay(power_data& s, float& fw)
	{
		weight* w = &fw;
		float rate_decay = 1.f;
		if (adaptive)
		{
			if (sqrt_rate)
				rate_decay = InvSqrt(w[adaptive]);
			else
				rate_decay = powf(w[adaptive], s.minus_power_t);
		}
		if (normalized)
		{
			if (sqrt_rate)
			{
				float inv_norm = 1.f / w[normalized];
				if (adaptive)
					rate_decay *= inv_norm;
				else
					rate_decay *= inv_norm*inv_norm;
			}
			else
				rate_decay *= powf(w[normalized] * w[normalized], s.neg_norm_power);
		}
		return rate_decay;
	}

	struct norm_data
	{
		float grad_squared;
		float pred_per_update;
		float norm_x;
		power_data pd;
	};

	const float x_min = 1.084202e-19f;
	const float x2_min = x_min*x_min;
	const float x2_max = FLT_MAX;
	
public:
	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare, bool stateless>
	// template<bool stateless>
	inline void pred_per_update_feature(norm_data& nd, float x, float& fw)
	{
		if (feature_mask_off || fw != 0.)
		{
			weight* w = &fw;
			float x2 = x * x;
			if (x2 < x2_min)
			{
				x = (x>0) ? x_min : -x_min;
				x2 = x2_min;
			}
			if (x2 > x2_max)
				THROW("your features have too much magnitude");
			if (adaptive && !stateless)
				w[adaptive] += nd.grad_squared * x2;
			if (normalized)
			{
				float x_abs = fabsf(x);
				if (x_abs > w[normalized] && !stateless)  // new scale discovered
				{
					if (w[normalized] > 0.)  //If the normalizer is > 0 then rescale the weight so it's as if the new scale was the old scale.
					{
						if (sqrt_rate)
						{
							float rescale = w[normalized] / x_abs;
							w[0] *= (adaptive ? rescale : rescale*rescale);
						}
						else
						{
							float rescale = x_abs / w[normalized];
							w[0] *= powf(rescale*rescale, nd.pd.neg_norm_power);
						}
					}
					w[normalized] = x_abs;
				}
				nd.norm_x += x2 / (w[normalized] * w[normalized]);
			}
			w[spare] = compute_rate_decay<sqrt_rate, adaptive, normalized>(nd.pd, fw);
			nd.pred_per_update += x2 * w[spare];
		}
	}
	
	//template<bool stateless>
	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare, bool stateless>
	inline void pred_per_update_feature2(float x, float& fw, float grad_squared, float pred_per_update,
	float norm_x, power_data pd)
	{
		if (feature_mask_off || fw != 0.)
		{
			weight* w = &fw;
			float x2 = x * x;
			if (x2 < x2_min)
			{
				x = (x>0) ? x_min : -x_min;
				x2 = x2_min;
			}
			if (x2 > x2_max)
				THROW("your features have too much magnitude");
			if (adaptive && !stateless)
				w[adaptive] += grad_squared * x2;
			if (normalized)
			{
				float x_abs = fabsf(x);
				if (x_abs > w[normalized] && !stateless)  // new scale discovered
				{
					if (w[normalized] > 0.)  //If the normalizer is > 0 then rescale the weight so it's as if the new scale was the old scale.
					{
						if (sqrt_rate)
						{
							float rescale = w[normalized] / x_abs;
							w[0] *= (adaptive ? rescale : rescale*rescale);
						}
						else
						{
							float rescale = x_abs / w[normalized];
							w[0] *= powf(rescale*rescale, pd.neg_norm_power);
						}
					}
					w[normalized] = x_abs;
				}
				norm_x += x2 / (w[normalized] * w[normalized]);
			}
			w[spare] = compute_rate_decay<sqrt_rate, adaptive, normalized>(pd, fw);
			pred_per_update += x2 * w[spare];
		}
	}

	bool global_print_features = false;
	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare, bool stateless>
	// template<bool stateless>
	float get_pred_per_update(example& ec)
	{ //We must traverse the features in _precisely_ the same order as during training.
		label_data& ld = ec.l.simple;
		float grad_squared = _all.loss->getSquareGrad(ec.pred.scalar, ld.label) * ec.weight;
		if (grad_squared == 0 && !stateless) return 1.;

		norm_data nd = { grad_squared, 0., 0.,{ _args.neg_power_t, _args.neg_norm_power } };
		//foreach_feature2<norm_data>(ec, nd, &GDLearnerBound::pred_per_update_feature<stateless>); //, pred_per_update_feature<stateless>
		// foreach_feature<norm_data, &GDLearnerBound::pred_per_update_feature<stateless>>(ec, nd); //, pred_per_update_feature<stateless>
		power_data pd = { _args.neg_power_t, _args.neg_norm_power };
		ff<float, float, float, power_data>::foreach_feature<&GDLearner::pred_per_update_feature2<sqrt_rate, feature_mask_off, adaptive, normalized, spare, stateless>>(*this, ec, grad_squared, 0., 0., pd);

		if (normalized)
		{
			if (!stateless)
			{
				_all.normalized_sum_norm_x += ec.weight * nd.norm_x;
				_args.total_weight += ec.weight;
			}
			_args.update_multiplier = average_update<sqrt_rate, adaptive, normalized>();
			nd.pred_per_update *= _args.update_multiplier;
		}
		return nd.pred_per_update;
	}

	//template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare, bool stateless>
	//template<bool stateless>
	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare, bool stateless>
	float sensitivity(example& ec)
	{
		if (adaptive || normalized)
			//return get_pred_per_update<stateless>(ec);
			return get_pred_per_update<sqrt_rate, feature_mask_off, adaptive, normalized, spare, stateless>(ec);
		else
			return ec.total_sum_feat_sq;
	}

	template<size_t adaptive>
	float get_scale(example& ec, float weight)
	{
		float update_scale = _all.eta * weight;
		if (!adaptive)
		{
			float t = (float)(_all.sd->t + weight - _all.sd->weighted_holdout_examples);
			update_scale *= powf(t, _args.neg_power_t);
		}
		return update_scale;
	}
public:
	template<bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	float sensitivity(example& ec, size_t)
	{
		return get_scale<adaptive>(ec, 1.)
			* sensitivity<sqrt_rate, feature_mask_off, adaptive, normalized, spare, true>(ec);
	}

private:
	//template<bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	template<bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	float compute_update(example& ec)
	{ //invariant: not a test label, importance weight > 0
		label_data& ld = ec.l.simple;

		float update = 0.;
		ec.updated_prediction = ec.pred.scalar;
		if (_all.loss->getLoss(_all.sd, ec.pred.scalar, ld.label) > 0.)
		{
			float pred_per_update = sensitivity<sqrt_rate, feature_mask_off, adaptive, normalized, spare, false>(ec);
			float update_scale = get_scale<adaptive>(ec, ec.weight);
			if (invariant)
				update = _all.loss->getUpdate(ec.pred.scalar, ld.label, update_scale, pred_per_update);
			else
				update = _all.loss->getUnsafeUpdate(ec.pred.scalar, ld.label, update_scale);
			// changed from ec.partial_prediction to ld.prediction
			ec.updated_prediction += pred_per_update * update;

			if (_all.reg_mode && fabs(update) > 1e-8)
			{
				double dev1 = _all.loss->first_derivative(_all.sd, ec.pred.scalar, ld.label);
				double eta_bar = (fabs(dev1) > 1e-8) ? (-update / dev1) : 0.0;
				if (fabs(dev1) > 1e-8)
					_all.sd->contraction *= (1. - _all.l2_lambda * eta_bar);
				update /= (float)_all.sd->contraction;
				_all.sd->gravity += eta_bar * _all.l1_lambda;
			}
		}

		if (sparse_l2)
			update -= sparse_l2 * ec.pred.scalar;

		return update;
	}

public:
	template<bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	void update(example& ec, size_t i = 0)
	{ //invariant: not a test label, importance weight > 0
		float update;
		if ((update = compute_update<sparse_l2, invariant, sqrt_rate, feature_mask_off, adaptive, normalized, spare>(ec)) != 0.)
			train<sqrt_rate, feature_mask_off, adaptive, normalized, spare>(ec, update);

		if (_all.sd->contraction < 1e-10)  // updating weights now to avoid numerical instability
			sync_weights(_all);
	}

	template<bool is_learn, bool l1, bool audit, bool sparse_l2, bool invariant, bool sqrt_rate, bool feature_mask_off, size_t adaptive, size_t normalized, size_t spare>
	//template<bool l1, bool is_learn>
	void predict_or_learn(example& ec, float& pred, float& label)
	{
		if (is_learn)
		{
			assert(ec.in_use);
			assert(ec.l.simple.label != FLT_MAX);
			assert(ec.weight > 0.);
		}

		// from gd.cc: predict
		if (l1)
			ec.partial_prediction = trunc_predict(ec, _all.sd->gravity);
		else
			ec.partial_prediction = inline_predict(ec);

		ec.partial_prediction *= (float)_all.sd->contraction;
		ec.pred.scalar = finalize_prediction(ec.partial_prediction);
		if (audit)
			print_audit_features(ec);

		if (is_learn)
			update<sparse_l2, invariant, sqrt_rate, feature_mask_off, adaptive, normalized, spare>(ec);
	}
};


Learner* GDArgs::create(vw& all)
{
	all.normalized_sum_norm_x = 0;
	no_win_counter = 0;
	total_weight = 0.;
	early_stop_thres = 3;
	// sparse_l2 = vm["sparse_l2"].as<float>();
	neg_norm_power = (all.adaptive ? (all.power_t - 1.f) : -1.f);
	neg_power_t = -all.power_t;
	adaptive = all.adaptive;
	normalized = all.normalized_updates; // TODO validate if this is not already set

	if (all.initial_t > 0)//for the normalized update: if initial_t is bigger than 1 we interpret this as if we had seen (all.initial_t) previous fake datapoints all with norm 1
	{
		all.normalized_sum_norm_x = all.initial_t;
		total_weight = all.initial_t;
	}

	// TODO: maybe move to options on top?
	po::variables_map& vm = all.vm;

	feature_mask_off = true;
	if (vm.count("feature_mask"))
		feature_mask_off = false;

	if (!all.holdout_set_off)
	{
		all.sd->holdout_best_loss = FLT_MAX;
		if (vm.count("early_terminate"))
			early_stop_thres = vm["early_terminate"].as< size_t>();
	}

	if (vm.count("constant"))
	{
		initial_constant = vm["constant"].as<float>();
	}

	if (vm.count("sgd") || vm.count("adaptive") || vm.count("invariant") || vm.count("normalized"))
	{ //nondefault
		all.adaptive = all.training && vm.count("adaptive");
		all.invariant_updates = all.training && vm.count("invariant");
		all.normalized_updates = all.training && vm.count("normalized");

		if (!vm.count("learning_rate") && !vm.count("l") && !(all.adaptive && all.normalized_updates))
			all.eta = 10; //default learning rate to 10 for non default update rule

							//if not using normalized or adaptive, default initial_t to 1 instead of 0
		if (!all.adaptive && !all.normalized_updates)
		{
			if (!vm.count("initial_t"))
			{
				all.sd->t = 1.f;
				all.sd->weighted_unlabeled_examples = 1.f;
				all.initial_t = 1.f;
			}
			all.eta *= powf((float)(all.sd->t), all.power_t);
		}
	}
	else
	{
		all.adaptive = all.training;
		all.invariant_updates = all.training;
		all.normalized_updates = all.training;
	}

	if (pow((double)all.eta_decay_rate, (double)all.numpasses) < 0.0001)
		cerr << "Warning: the learning rate for the last pass is multiplied by: " << pow((double)all.eta_decay_rate, (double)all.numpasses)
		<< " adjust --decay_learning_rate larger to avoid this." << endl;

	l1 = false, audit = false;
	if (all.reg_mode % 2)
		if (all.audit || all.hash_inv)
		{
			l1 = true;
			audit = true;
		}
		else
		{
			l1 = true;
			audit = false;
		}
	else if (all.audit || all.hash_inv)
	{
		l1 = false;
		audit = true;
	}

	sqrt_rate = all.power_t == 0.5;

	invariant = all.invariant_updates;
	sparse_l2_t = sparse_l2 > 0.f;

	reg_mode = all.reg_mode;
	hash_inv = all.hash_inv;

	return new GDLearner(*this, all);
}

// 2 step creation process
// - create factory first
// - factory creates actual learn instance, allows for templatization of learner based on command args

IArguments* gd_create() { return new GDArgs; }