//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// nested_loop_join_executor.cpp
//
// Identification: src/executor/nested_loop_join_executor.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <vector>
#include <unordered_set>

#include "common/types.h"
#include "common/logger.h"
#include "executor/nested_loop_join_executor.h"
#include "executor/executor_context.h"
#include "executor/index_scan_executor.h"
#include "planner/nested_loop_join_plan.h"
#include "planner/index_scan_plan.h"
#include "expression/abstract_expression.h"
#include "expression/tuple_value_expression.h"
#include "common/container_tuple.h"

namespace peloton {
namespace executor {

/**
 * @brief Constructor for nested loop join executor.
 * @param node Nested loop join node corresponding to this executor.
 */
NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    const planner::AbstractPlan *node, ExecutorContext *executor_context)
    : AbstractJoinExecutor(node, executor_context) {}

/**
 * @brief Do some basic checks and create the schema for the output logical
 * tiles.
 * @return true on success, false otherwise.
 */
bool NestedLoopJoinExecutor::DInit() {
  auto status = AbstractJoinExecutor::DInit();
  if (status == false) {
    return status;
  }

  PL_ASSERT(right_result_tiles_.empty());
  right_child_done_ = false;
  right_result_itr_ = 0;

  PL_ASSERT(left_result_tiles_.empty());

  return true;
}

/**
 * @brief Creates logical tiles from the two input logical tiles after applying
 * join predicate.
 * @return true on success, false otherwise.
 *
 * ExecutorContext is set when executing IN+NestLoop. For example:
 * select * from Foo1 where age IN (select id from Foo2 where name='mike');
 * Here:
 * "select id from Foo2 where name='mike'" is transformed as left child.
 * "select * from Foo1 where age " is the right child.
 * "IN" is transformed as a execute context, in NestLoop
 * We put the results of left child in executor_context using NestLoop, and the
 * right child can execute using this context. Otherwise, the right child can't
 * execute. And there is no predicate_ for IN+NestLoop
 *
 * For now, we only set this context for IN operator. Normally, the right child
 * has a complete query that can execute without the context, and we use
 *predicate_
 * to join the left and right result.
 *
 */
bool NestedLoopJoinExecutor::Old_DExecute() {
  LOG_TRACE("********** Nested Loop %s Join executor :: 2 children ",
            GetJoinTypeString());

  // Loop until we have non-empty result tile or exit
  for (;;) {
    // Build outer join output when done
    if (left_child_done_ && right_child_done_) {
      return BuildOuterJoinOutput();
    }

    //===------------------------------------------------------------------===//
    // Pick left and right tiles
    //===------------------------------------------------------------------===//

    LogicalTile *left_tile = nullptr;
    LogicalTile *right_tile = nullptr;

    bool advance_right_child = false;

    // If we have already retrieved all left child's results in buffer
    if (left_child_done_ == true) {
      LOG_TRACE("Advance the left buffer iterator.");

      PL_ASSERT(!right_result_tiles_.empty());
      left_result_itr_++;

      if (left_result_itr_ >= left_result_tiles_.size()) {
        advance_right_child = true;
        left_result_itr_ = 0;
      }

    }
    // Otherwise, we must attempt to execute the left child
    else {
      // Left child is finished, no more tiles
      if (children_[0]->Execute() == false) {
        LOG_TRACE("Left child is exhausted.");

        left_child_done_ = true;
        left_result_itr_ = 0;
        advance_right_child = true;
      }
      // Buffer the left child's result
      else {
        LOG_TRACE("Retrieve a new tile from left child");
        BufferLeftTile(children_[0]->GetOutput());
        left_result_itr_ = left_result_tiles_.size() - 1;
      }
    }

    if (advance_right_child == true || right_result_tiles_.empty()) {
      // return if right tile is empty
      if (right_child_done_ && right_result_tiles_.empty()) {
        return BuildOuterJoinOutput();
      }

      PL_ASSERT(left_result_itr_ == 0);

      // Right child is finished, no more tiles
      if (children_[1]->Execute() == false) {
        LOG_TRACE("Right child is exhausted. Returning false.");

        // Right child exhausted.
        // Release cur Right tile. Clear right child's result buffer and return.
        right_child_done_ = true;

        return BuildOuterJoinOutput();
      }
      // Buffer the Right child's result
      else {
        LOG_TRACE("Advance the Right child.");
        BufferRightTile(children_[1]->GetOutput());
        // return if left tile is empty
        if (left_child_done_ && left_result_tiles_.empty()) {
          return BuildOuterJoinOutput();
        }
      }
    }

    right_tile = right_result_tiles_.back().get();
    left_tile = left_result_tiles_[left_result_itr_].get();

    //===------------------------------------------------------------------===//
    // Build Join Tile
    //===------------------------------------------------------------------===//

    // Build output logical tile
    auto output_tile = BuildOutputLogicalTile(left_tile, right_tile);

    // Build position lists
    LogicalTile::PositionListsBuilder pos_lists_builder(left_tile, right_tile);

    // Go over every pair of tuples in left and right logical tiles
    for (auto right_tile_row_itr : *right_tile) {
      bool has_left_match = false;

      for (auto left_tile_row_itr : *left_tile) {
        // Join predicate exists
        if (predicate_ != nullptr) {
          expression::ContainerTuple<executor::LogicalTile> left_tuple(
              left_tile, left_tile_row_itr);
          expression::ContainerTuple<executor::LogicalTile> right_tuple(
              right_tile, right_tile_row_itr);

          // Join predicate is false. Skip pair and continue.
          auto eval = predicate_->Evaluate(&left_tuple, &right_tuple,
                                           executor_context_);
          if (eval.IsFalse()) {
            continue;
          }
        }

        RecordMatchedLeftRow(left_result_itr_, left_tile_row_itr);

        // For Left and Full Outer Join
        has_left_match = true;

        // Insert a tuple into the output logical tile
        // First, copy the elements in left logical tile's tuple
        pos_lists_builder.AddRow(left_tile_row_itr, right_tile_row_itr);
      }  // Inner loop of NLJ

      // For Right and Full Outer Join
      if (has_left_match) {
        RecordMatchedRightRow(right_result_tiles_.size() - 1,
                              right_tile_row_itr);
      }

    }  // Outer loop of NLJ

    // Check if we have any join tuples.
    if (pos_lists_builder.Size() > 0) {
      output_tile->SetPositionListsAndVisibility(pos_lists_builder.Release());
      SetOutput(output_tile.release());
      return true;
    }

    LOG_TRACE("This pair produces empty join result. Continue the loop.");
  }  // end the very beginning for loop
}

bool NestedLoopJoinExecutor::DExecute() {
  LOG_TRACE("********** Nested Loop %s Join executor :: 2 children ",
            GetJoinTypeString());

  // Loop until we have non-empty result tile or exit
  for (;;) {
    // Build outer join output when done
    if (left_child_done_ && right_child_done_) {
      return BuildOuterJoinOutput();
    }

    //===------------------------------------------------------------------===//
    // Pick left and right tiles
    //===------------------------------------------------------------------===//

    LogicalTile *left_tile = nullptr;
    LogicalTile *right_tile = nullptr;

    bool advance_right_child = false;

    // If we have already retrieved all left child's results in buffer
    if (left_child_done_ == true) {
      LOG_TRACE("Advance the left buffer iterator.");

      PL_ASSERT(!right_result_tiles_.empty());
      left_result_itr_++;

      if (left_result_itr_ >= left_result_tiles_.size()) {
        advance_right_child = true;
        left_result_itr_ = 0;
      }
    }
    // Otherwise, we must attempt to execute the left child
    else {
      // Left child is finished, no more tiles
      if (children_[0]->Execute() == false) {
        LOG_TRACE("Left child is exhausted.");

        left_child_done_ = true;
        left_result_itr_ = 0;
        advance_right_child = true;
      }
      // Buffer the left child's result
      else {
        LOG_TRACE("Retrieve a new tile from left child");
        BufferLeftTile(children_[0]->GetOutput());
        left_result_itr_ = left_result_tiles_.size() - 1;
      }
    }

    left_tile = left_result_tiles_[left_result_itr_].get();

    for (auto left_tile_row_itr : *left_tile) {
      // Tuple result
      expression::ContainerTuple<executor::LogicalTile> left_tuple(
          left_tile, left_tile_row_itr);

      // Pick out the join predicate column value for the left table.
      // TODO: There might be multiple predicates
      oid_t predicate_coloumn =
          ((expression::TupleValueExpression *)predicate_->GetLeft())
              ->GetColumnId();
      common::Value predicate_value = left_tuple.GetValue(predicate_coloumn);

      // Put this value into right child
      // TODO: Adding multiple predicates and values
      if (children_[1]->GetRawNode()->GetPlanNodeType() ==
          PLAN_NODE_TYPE_INDEXSCAN) {
        std::cout << "binggo" << std::endl;
        bool replace =
            ((executor::IndexScanExecutor *)children_[1])
                ->GetPlan()
                ->ReplaceKeyValue(predicate_coloumn, predicate_value);
        if (replace == false) {
          LOG_TRACE("Error comparison in Nested Loop.");
          return false;
        }
      }

      // Lookup right

      // return if right tile is empty
      if (right_child_done_ && right_result_tiles_.empty()) {
        return BuildOuterJoinOutput();
      }

      PL_ASSERT(left_result_itr_ == 0);

      // Right child is finished, no more tiles
      for (;;) {
        if (children_[1]->Execute() == true) {
          LOG_TRACE("Advance the Right child.");
          BufferRightTile(children_[1]->GetOutput());

          // return if left tile is empty
          if (left_child_done_ && left_result_tiles_.empty()) {
            return BuildOuterJoinOutput();
          }
        }
        // Right is finished
        else {
          if (!left_child_done_) {
            // TODO: We should add type judgement, like IndexScan or SeqScan
            ((executor::IndexScanExecutor *)children_[1])->ResetState();
          } else {
            right_child_done_ = true;
          }
          break;
        }
      }  // End for
    }    // Buffered all results

    if (advance_right_child == true || right_result_tiles_.empty()) {
      // return if right tile is empty
      if (right_child_done_ && right_result_tiles_.empty()) {
        return BuildOuterJoinOutput();
      }

      PL_ASSERT(left_result_itr_ == 0);
    }

    right_tile = right_result_tiles_.back().get();

    //===------------------------------------------------------------------===//
    // Build Join Tile
    //===------------------------------------------------------------------===//

    // Build output logical tile
    auto output_tile = BuildOutputLogicalTile(left_tile, right_tile);

    // Build position lists
    LogicalTile::PositionListsBuilder pos_lists_builder(left_tile, right_tile);

    // Go over every pair of tuples in left and right logical tiles
    for (auto right_tile_row_itr : *right_tile) {
      bool has_left_match = false;

      for (auto left_tile_row_itr : *left_tile) {
        // Join predicate exists
        if (predicate_ != nullptr) {
          expression::ContainerTuple<executor::LogicalTile> left_tuple(
              left_tile, left_tile_row_itr);
          expression::ContainerTuple<executor::LogicalTile> right_tuple(
              right_tile, right_tile_row_itr);

          // Join predicate is false. Skip pair and continue.
          auto eval = predicate_->Evaluate(&left_tuple, &right_tuple,
                                           executor_context_);
          if (eval.IsFalse()) {
            continue;
          }
        }

        RecordMatchedLeftRow(left_result_itr_, left_tile_row_itr);

        // For Left and Full Outer Join
        has_left_match = true;

        // Insert a tuple into the output logical tile
        // First, copy the elements in left logical tile's tuple
        pos_lists_builder.AddRow(left_tile_row_itr, right_tile_row_itr);
      }  // Inner loop of NLJ

      // For Right and Full Outer Join
      if (has_left_match) {
        RecordMatchedRightRow(right_result_tiles_.size() - 1,
                              right_tile_row_itr);
      }

    }  // Outer loop of NLJ

    // Check if we have any join tuples.
    if (pos_lists_builder.Size() > 0) {
      output_tile->SetPositionListsAndVisibility(pos_lists_builder.Release());
      SetOutput(output_tile.release());
      return true;
    }

    LOG_TRACE("This pair produces empty join result. Continue the loop.");
  }  // end the very beginning for loop
}

// bool NestedLoopJoinExecutor::DExecute() {
//  LOG_TRACE("********** Nested Loop %s Join executor :: 2 children ",
//            GetJoinTypeString());
//
//  // Loop until we have non-empty result tile or exit
//  // TODO: How to loop all tiles?
//  for (;;) {
//
//    LogicalTile *left_tile = nullptr;
//    LogicalTile *right_tile = nullptr;
//
//    // Build output logical tile
//    auto output_tile = BuildOutputLogicalTile(left_tile, right_tile);
//    // Build position lists
//    LogicalTile::PositionListsBuilder pos_lists_builder(left_tile,
// right_tile);
//
//    // TODO: What means if execute if false. How to deal with that?
//    if (children_[0]->Execute() == false) {
//      return false;
//    }
//    // Success get the result of left
//    else {
//      // For each tuple in the result, pick out the predicate value
//      for (auto left_itr : *(children_[0]->GetOutput())) {
//        // Tuple result
//        expression::ContainerTuple<executor::LogicalTile>
// left_tuple(left_tile,
//                                                                     left_itr);
//
//        // Pick out the join predicate column value for the left table.
//        // TODO: There might be multiple predicates
//        oid_t predicate_coloumn =
//            ((expression::TupleValueExpression *)predicate_->GetLeft())
//                ->GetColumnId();
//
//        common::Value predicate_value =
// left_tuple.GetValue(predicate_coloumn);
//
//        // Put this value into right child
//        // TODO: Adding multiple predicates and values
//        if (children_[1]->GetRawNode()->GetPlanNodeType() ==
//            PLAN_NODE_TYPE_INDEXSCAN) {
//          ((planner::IndexScanPlan *)children_[1]->GetRawNode())
//              ->ReplaceKeyValue(predicate_coloumn, predicate_value);
//        }
//
//        // Loop right child and buffer result
//        while (children_[1]->Execute() == true) {
//          // For each right result, combine them in output tile
//          for (auto right_itr : *(children_[1]->GetOutput())) {
//            // Combine them in output tile
//            pos_lists_builder.AddRow(left_itr, right_itr);
//          }
//        }  // End right loop
//      }
//
//      // TODO: pos_lists_builder is ok here?
//      if (pos_lists_builder.Size() > 0) {
//        output_tile->SetPositionListsAndVisibility(pos_lists_builder.Release());
//        SetOutput(output_tile.release());
//        return true;
//      } else {
//        return false;
//      }
//    }
//
//  }  // end the very beginning for loop
//}

}  // namespace executor
}  // namespace peloton
