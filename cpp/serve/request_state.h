/*!
 *  Copyright (c) 2023 by Contributors
 * \file serve/request_state.h
 * \brief The data structure maintaining the generation states of user requests.
 */
#ifndef MLC_LLM_SERVE_REQUEST_STATE_H_
#define MLC_LLM_SERVE_REQUEST_STATE_H_

#include <tvm/runtime/container/array.h>
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/object.h>

#include <optional>

#include "../streamer.h"
#include "../support/random.h"
#include "config.h"
#include "grammar/grammar_state_matcher.h"
#include "request.h"

namespace mlc {
namespace llm {
namespace serve {

using namespace tvm::runtime;

/*!
 * \brief The state of a request with regard to some single model.
 * \details In MLC LLM, the serving engine may leverage multiple models
 * to fulfill a user generation request (e.g., use speculation decoding).
 * For each request, we isolate its states (e.g. the generated tokens)
 * on each model. This is to say, we use RequestModelState to store
 * the state of a user request on a single model (rather than all models).
 */
class RequestModelStateNode : public Object {
 public:
  /*! \brief The request that this state corresponds to. */
  Request request;
  /*!
   * \brief The internal request id of this state.
   * It is the **physical index** of the request in the running request queue.
   * If the request is on hold (not in the running queue), the request id
   * should be -1.
   */
  int64_t internal_id = -1;
  /*! \brief The corresponding model id of this state. */
  int model_id = -1;
  /*!
   * \brief The committed generated token ids and related probability info.
   * A token is "committed" means it will no longer be updated (or changed).
   */
  std::vector<SampleResult> committed_tokens;
  /*! \brief The list of input data yet for the model to prefill. */
  Array<Data> inputs;
  /*! \brief The list of prefilled input data, used to notify prefix cache. */
  Array<Data> prefilled_inputs;
  /*! \brief The number of tokens already cached in prefix cache. */
  int64_t cached_committed_tokens = 0;
  /*! \brief The number of tokens that is already prefilled from the inputs. */
  int64_t num_prefilled_tokens = 0;

  // NOTE: The following fields are reserved for future speculative inference
  // settings, and are produced by the speculative small models.
  /*!
   * \brief The draft generated token ids and related probability info,
   * which are usually generated by "small" speculative models.
   * These tokens will be fed to a "large" model to determine the final
   * result of speculation.
   */
  std::vector<SampleResult> draft_output_tokens;
  /*! \brief The storage slots for the associated states of draft tokens. */
  std::vector<int> draft_token_slots;
  /*! \brief The appeared committed and draft tokens and their occurrence times. */
  std::unordered_map<int32_t, int32_t> appeared_token_ids;

  /*!
   * \brief The current state of the generated token matching the grammar. Used in grammar-guided
   * generation, otherwise it's NullOpt.
   */
  Optional<GrammarStateMatcher> grammar_state_matcher;

  /*! \brief Return the total length of the input data. */
  int GetInputLength() const;
  /*!
   * \brief Return whether the next token bitmask is required, i.e. the grammar-guided generation is
   * enabled.
   */
  bool RequireNextTokenBitmask();
  /*!
   * \brief Find the next token bitmask and store it in the given DLTensor.
   * \param bitmask The DLTensor to store the next token bitmask. The bitmask should be a tensor
   * with dtype uint32_t and shape (ceildiv(vocab_size, 32),).
   */
  void FindNextTokenBitmask(DLTensor* bitmask);
  /*! \brief Commit a new token into committed_tokens. Update appeared_token_ids. */
  void CommitToken(SampleResult sampled_token);
  /*! \brief Add a draft token into draft_output_tokens. Update appeared_token_ids. */
  void AddDraftToken(SampleResult sampled_token, int draft_token_slot);
  /*! \brief Remove all draft tokens from draft_output_tokens. Update appeared_token_ids. */
  void RemoveAllDraftTokens(std::vector<int>* removed_draft_token_slots = nullptr);

  static constexpr const char* _type_key = "mlc.serve.RequestModelState";
  static constexpr const bool _type_has_method_sequal_reduce = false;
  static constexpr const bool _type_has_method_shash_reduce = false;
  TVM_DECLARE_BASE_OBJECT_INFO(RequestModelStateNode, Object);

 private:
  /*! \brief Remove the last token from draft_output_tokens. Update appeared_token_ids. */
  void RemoveLastDraftToken();
};

class RequestModelState : public ObjectRef {
 public:
  explicit RequestModelState(
      Request request, int model_id, int64_t internal_id, Array<Data> inputs,
      const std::optional<std::shared_ptr<GrammarStateInitContext>>& grammar_state_init_ctx);

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(RequestModelState, ObjectRef, RequestModelStateNode);
};

struct DeltaRequestReturn {
  std::vector<int32_t> delta_token_ids;
  Array<String> delta_logprob_json_strs;
  Optional<String> finish_reason;
};

/****************** Request States ******************/

/*!
 * \brief For each request, we maintain its "request state" in the
 * engine. Generally, the state of a request contains the information
 * of the request's generation at the current moment, including
 * the generated token ids, the grammar handler, etc.
 *
 * When a request has multiple parallel generations (e.g., the field
 * `n` of its generation config is more than 1), each generation will
 * have different states all the time.
 *
 * Therefore, to better support parallel generations, we denote the
 * state of a single generation as a "RequestStateEntry" instance,
 * and denote the state of a request's all generations using a vector,
 * named as a "RequestState" instance.
 *
 * A request's all state entries are organized as a tree structure
 * when there are parallel generations.
 * - the request input has the root status entry,
 * - each parallel generation is a child of the root.
 * This tree structure may be further extended to more complicated
 * cases in the future. As of now, for the case of `n > 1`, there
 * will be (n + 1) entries in total. In a "RequestState", the root
 * entry always has index 0. And we guarantee that the entry order
 * from the vector begin to the end is always a topological order
 * of the tree.
 */

/*! \brief Request state status. */
enum class RequestStateStatus : int {
  kPending = 0,
  kAlive = 1,
  kFinished = 2,
};

/*!
 * \brief A request's state entry. It contains the state of a single
 * generation of a request, or the state of a prompt prefix of a request.
 */
class RequestStateEntryNode : public Object {
 public:
  /*! \brief The status of the request state entry. */
  RequestStateStatus status;
  /*! \brief The request that this state corresponds to. */
  Request request;
  /*!
   * \brief The idx of the parent request state entry of this state.
   * Being -1 means the state has no parent and is the foremost
   * "prefix" entry or the only entry.
   */
  int parent_idx = -1;
  /*! \brief The children indices of the request state entry. */
  std::vector<int> child_indices;

  /*!
   * \brief The state with regard to each model.
   * \sa RequestModelState
   */
  Array<RequestModelState> mstates;
  /*! \brief The random number generator of this request state entry. */
  RandomGenerator rng;
  /*! \brief The stop string handler of this request state entry. */
  StopStrHandler stop_str_handler;
  /*!
   * \brief The start position of the committed tokens in the
   * next request stream callback invocation.
   */
  int next_callback_token_pos;

  /*! \brief The time of adding the request to engine. */
  std::chrono::high_resolution_clock::time_point tadd;
  /*! \brief The time of finishing prefill stage. */
  std::chrono::high_resolution_clock::time_point tprefill_finish;

  /*!
   * \brief Get the delta token ids and the logprob JSON strings for this request to return since
   * the last time calling into this function, and return the finish reason if the request
   * generation has finished.
   * \param tokenizer The tokenizer for logprob process.
   * \param max_single_sequence_length The maximum allowed single sequence length.
   * \return The delta token ids to return, the logprob JSON strings of each delta token id, and
   * the optional finish reason.
   */
  DeltaRequestReturn GetReturnTokenIds(const Tokenizer& tokenizer,
                                       int64_t max_single_sequence_length);

  static constexpr const char* _type_key = "mlc.serve.RequestStateEntry";
  static constexpr const bool _type_has_method_sequal_reduce = false;
  static constexpr const bool _type_has_method_shash_reduce = false;
  TVM_DECLARE_FINAL_OBJECT_INFO(RequestStateEntryNode, Object);
};

class RequestStateEntry : public ObjectRef {
 public:
  explicit RequestStateEntry(
      Request request, int num_models, int64_t internal_id, int rng_seed,
      const std::vector<std::string>& token_table,
      const std::optional<std::shared_ptr<GrammarStateInitContext>>& grammar_state_init_ctx,
      int parent_idx = -1);

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(RequestStateEntry, ObjectRef, RequestStateEntryNode);
};

/*! \brief A request's state, which groups all the request state entries. */
class RequestStateNode : public Object {
 public:
  std::vector<RequestStateEntry> entries;

  static constexpr const char* _type_key = "mlc.serve.RequestState";
  static constexpr const bool _type_has_method_sequal_reduce = false;
  static constexpr const bool _type_has_method_shash_reduce = false;
  TVM_DECLARE_FINAL_OBJECT_INFO(RequestStateNode, Object);
};

class RequestState : public ObjectRef {
 public:
  explicit RequestState(std::vector<RequestStateEntry> entries);

  TVM_DEFINE_MUTABLE_OBJECT_REF_METHODS(RequestState, ObjectRef, RequestStateNode);
};

}  // namespace serve
}  // namespace llm
}  // namespace mlc

#endif  // MLC_LLM_SERVE_REQUEST_STATE_H_
