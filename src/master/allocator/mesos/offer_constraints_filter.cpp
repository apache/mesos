// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/variant.hpp>

#include <re2/re2.h>

#include <mesos/allocator/allocator.hpp>
#include <mesos/attributes.hpp>

using std::string;
using std::vector;
using std::unique_ptr;
using std::unordered_map;

using ::mesos::scheduler::AttributeConstraint;
using ::mesos::scheduler::OfferConstraints;

using RE2Limits =
  ::mesos::allocator::OfferConstraintsFilter::Options::RE2Limits;

namespace mesos {
namespace allocator {

namespace internal {

// TODO(asekretenko): Cache returned RE2s to make sure that:
//  - a RE2 corresponding to a pattern is not re-created needlessly
//  - multiple RE2s corresponding to the same pattern don't coexist
static Try<unique_ptr<const RE2>> createRE2(
    const RE2Limits& limits,
    const string& regex)
{
  RE2::Options options{RE2::CannedOptions::Quiet};
  options.set_max_mem(limits.maxMem.bytes());
  unique_ptr<const RE2> re2{new RE2(regex, options)};

  if (!re2->ok()) {
    return Error(
        "Failed to construct regex from pattern"
        " '" + regex + "': " + re2->error());
  }

  if (re2->ProgramSize() > limits.maxProgramSize) {
    return Error(
        "Regex '" + regex + "' is too complex: program size of " +
        stringify(re2->ProgramSize()) + " is larger than maximum of " +
        stringify(limits.maxProgramSize) + " allowed");
  }

  // Without `std::move`, pre-8.0.0 gcc and pre-3.9.0 clang deduce the type of
  // `T` in `template<T> Try<>::Try(T&&)` to be `unique_ptr&`, not `unique_ptr`.
  return std::move(re2);
}


using Selector = AttributeConstraint::Selector;


static Option<Error> validate(const Selector& selector)
{
  switch(selector.selector_case()) {
    case Selector::kPseudoattributeType:
      switch (selector.pseudoattribute_type()) {
        case Selector::HOSTNAME:
        case Selector::REGION:
        case Selector::ZONE:
          return None();
        case Selector::UNKNOWN:
          break;
      }

      return Error("Unknown pseudoattribute type");

    case Selector::kAttributeName:
      return None();

    case Selector::SELECTOR_NOT_SET:
      return Error(
        "Exactly one of 'AttributeConstraint::Selector::name' or"
        " 'AttributeConstraint::Selector::pseudoattribute_type' must be set");
  }

  UNREACHABLE();
}


class AttributeConstraintPredicate
{
public:
  bool apply(const Nothing& _) const { return apply_(_); }
  bool apply(const string& str) const { return apply_(str); }
  bool apply(const Attribute& attr) const { return apply_(attr); }

  static Try<AttributeConstraintPredicate> create(
      const RE2Limits& re2Limits,
      AttributeConstraint::Predicate&& predicate)
  {
    using Self = AttributeConstraintPredicate;

    switch (predicate.predicate_case()) {
      case AttributeConstraint::Predicate::kExists:
        return Self(Exists{});
      case AttributeConstraint::Predicate::kNotExists:
        return Self(NotExists{});

      case AttributeConstraint::Predicate::kTextEquals:
        return Self(TextEquals{
          std::move(*predicate.mutable_text_equals()->mutable_value())});

      case AttributeConstraint::Predicate::kTextNotEquals:
        return Self(TextNotEquals{
          std::move(*predicate.mutable_text_not_equals()->mutable_value())});

      case AttributeConstraint::Predicate::kTextMatches: {
        Try<unique_ptr<const RE2>> re2 =
          createRE2(re2Limits, predicate.text_matches().regex());

        if (re2.isError()) {
          return Error(re2.error());
        }

        return Self(TextMatches{std::move(*re2)});
      }

      case AttributeConstraint::Predicate::kTextNotMatches: {
        Try<unique_ptr<const RE2>> re2 =
          createRE2(re2Limits, predicate.text_not_matches().regex());

        if (re2.isError()) {
          return Error(re2.error());
        }

        return Self(TextNotMatches{std::move(*re2)});
      }

      case AttributeConstraint::Predicate::PREDICATE_NOT_SET:
        return Error("Unknown predicate type");
    }
    UNREACHABLE();
  }

private:
  // The following helper structs apply the predicate using
  // overloads for the three cases:
  //   (1) Nothing   -> non existent (pseudo) attribute
  //   (2) string    -> pseudo attribute value
  //   (3) Attribute -> named attribute value
  struct Exists
  {
    bool apply(const Nothing&) const { return false; }
    bool apply(const string&) const { return true; }
    bool apply(const Attribute&) const { return true; }
  };

  struct NotExists
  {
    bool apply(const Nothing&) const { return true; }
    bool apply(const string&) const { return false; }
    bool apply(const Attribute&) const { return false; }
  };

  struct TextEquals
  {
    string value;

    bool apply(const Nothing&) const { return false; }
    bool apply(const string& str) const { return str == value; }
    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT || attr.text().value() == value;
    }
  };

  struct TextNotEquals
  {
    string value;

    bool apply(const Nothing&) const { return true; }
    bool apply(const string& str) const { return str != value; }
    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT || attr.text().value() != value;
    }
  };

  struct TextMatches
  {
    unique_ptr<const RE2> re2;

    bool apply(const Nothing&) const { return false; }
    bool apply(const string& str) const { return RE2::FullMatch(str, *re2); }

    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT ||
             RE2::FullMatch(attr.text().value(), *re2);
    }
  };

  struct TextNotMatches
  {
    unique_ptr<const RE2> re2;

    bool apply(const Nothing&) const { return true; }
    bool apply(const string& str) const { return !RE2::FullMatch(str, *re2); }

    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT ||
             !RE2::FullMatch(attr.text().value(), *re2);
    }
  };


  using Predicate = Variant<
      Nothing,
      Exists,
      NotExists,
      TextEquals,
      TextNotEquals,
      TextMatches,
      TextNotMatches>;

  Predicate predicate;

  AttributeConstraintPredicate(Predicate&& p) : predicate(std::move(p)){};

  template <class T>
  bool apply_(const T& attribute) const
  {
    return predicate.visit(
        [](const Nothing& p) -> bool {
          LOG(FATAL) << "Predciate not initialized properly";
          UNREACHABLE();
        },
        [&](const Exists& p) { return p.apply(attribute); },
        [&](const NotExists& p) { return p.apply(attribute); },
        [&](const TextEquals& p) { return p.apply(attribute); },
        [&](const TextNotEquals& p) { return p.apply(attribute); },
        [&](const TextMatches& p) { return p.apply(attribute); },
        [&](const TextNotMatches& p) { return p.apply(attribute); });
  }
};


class AttributeConstraintEvaluator
{
public:
  bool evaluate(const SlaveInfo& info) const
  {
    switch (selector.selector_case()) {
      case Selector::kAttributeName: {
        const string& name = selector.attribute_name();
        const auto attr = std::find_if(
            info.attributes().cbegin(),
            info.attributes().cend(),
            [&name](const Attribute& a) { return a.name() == name; });

        return attr == info.attributes().cend() ? predicate.apply(Nothing())
                                                : predicate.apply(*attr);
      }

      case Selector::kPseudoattributeType:
        switch (selector.pseudoattribute_type()) {
          case Selector::HOSTNAME:
            return predicate.apply(info.hostname());

          case Selector::REGION:
            return info.has_domain() && info.domain().has_fault_domain()
                ? predicate.apply(info.domain().fault_domain().region().name())
                : predicate.apply(Nothing());

          case Selector::ZONE:
            return info.has_domain() && info.domain().has_fault_domain()
                ? predicate.apply(info.domain().fault_domain().zone().name())
                : predicate.apply(Nothing());

          case Selector::UNKNOWN:
            LOG(FATAL) << "Unknown pseudoattribute value passed validation";
        }

        UNREACHABLE();

      case Selector::SELECTOR_NOT_SET:
        LOG(FATAL) << "'AttributeConstraint::Selector::selector' oneof that"
                      " has no known value set passed validation";
    }

    UNREACHABLE();
  }

  static Try<AttributeConstraintEvaluator> create(
      const RE2Limits& re2Limits,
      AttributeConstraint&& constraint)
  {
    Option<Error> error = validate(constraint.selector());
    if (error.isSome()) {
      return *error;
    }

    Try<AttributeConstraintPredicate> predicate =
      AttributeConstraintPredicate::create(
          re2Limits, std::move(*constraint.mutable_predicate()));

    if (predicate.isError()) {
      return Error(predicate.error());
    }

    return AttributeConstraintEvaluator{
      std::move(*constraint.mutable_selector()), std::move(*predicate)};
  }

private:
  Selector selector;
  AttributeConstraintPredicate predicate;

  AttributeConstraintEvaluator(
      Selector&& selector_,
      AttributeConstraintPredicate&& predicate_)
    : selector(std::move(selector_)),
      predicate(std::move(predicate_))
  {}
};


class OfferConstraintsFilterImpl
{
  using Group = vector<AttributeConstraintEvaluator>;

public:
  OfferConstraintsFilterImpl(
      unordered_map<string, vector<Group>>&& expressions_)
    : expressions(std::move(expressions_))
  {}

  bool isAgentExcluded(const std::string& role, const SlaveInfo& info) const
  {
    auto roleConstraintsExpression = expressions.find(role);
    if (roleConstraintsExpression == expressions.end()) {
      return false;
    }

    // TODO(asekretenko): This method evaluates the constraints in the order in
    // which they have been passed by the scheduler. Given that agents are
    // seldom added or have their (pseudo)attributes changed, tracking
    // match/mismatch frequency and runtime cost of the constraints, and
    // reordering the expression accordingly (so that the cheapest groups that
    // usually match come first, and the most expensive that usually don't come
    // last) could potentially help speed up this method.

    return !std::any_of(
        roleConstraintsExpression->second.cbegin(),
        roleConstraintsExpression->second.cend(),
        [&info](const Group& group) {
          return std::all_of(
              group.cbegin(),
              group.cend(),
              [&info](const AttributeConstraintEvaluator& e) {
                return e.evaluate(info);
              });
        });
  }

  static Try<OfferConstraintsFilterImpl> create(
      const OfferConstraintsFilter::Options& options,
      OfferConstraints&& constraints)
  {
    // TODO(asekretenko): This method performs a dumb 1:1 translation of
    // `AttributeConstraint`s without any reordering; this leaves room for
    // a number of potential optimizations such as:
    //  - deactivating a framework with no constraint groups
    //  - constructing no filter if there is a single empty group
    //  - deduplicating constraints and groups
    //  - reordering constraints and groups so that the potentially cheaper ones
    //    come first
    using Group = vector<AttributeConstraintEvaluator>;
    unordered_map<string, vector<Group>> expressions;

    for (auto& pair : *constraints.mutable_role_constraints()) {
      const string& role = pair.first;
      OfferConstraints::RoleConstraints& roleConstraints = pair.second;

      if (roleConstraints.groups().empty()) {
        return Error(
            "'OfferConstraints::role_constraints' has an empty "
            "'RoleConstraints::groups' for role " +
            role);
      }

      vector<Group>& groups = expressions[role];

      for (OfferConstraints::RoleConstraints::Group& group_ :
           *roleConstraints.mutable_groups()) {
        if (group_.attribute_constraints().empty()) {
          return Error(
              "'OfferConstraints::RoleConstraints::groups' for role " + role +
              "contains an empty RoleConstraints::Group");
        }

        groups.emplace_back();
        Group& group = groups.back();

        for (AttributeConstraint& constraint :
             *group_.mutable_attribute_constraints()) {
          Try<AttributeConstraintEvaluator> evaluator =
            AttributeConstraintEvaluator::create(
                options.re2Limits, std::move(constraint));

          if (evaluator.isError()) {
            return Error(
                "A role " + role +
                " has an invalid 'AttributeConstraint': " + evaluator.error());
          }

          group.emplace_back(std::move(*evaluator));
        }
      }
    }

    return OfferConstraintsFilterImpl(std::move(expressions));
  }

private:
  unordered_map<string, vector<Group>> expressions;
};

} // namespace internal {


using internal::OfferConstraintsFilterImpl;


Try<OfferConstraintsFilter> OfferConstraintsFilter::create(
    const Options& options,
    OfferConstraints&& constraints)
{
  Try<OfferConstraintsFilterImpl> impl =
    OfferConstraintsFilterImpl::create(options, std::move(constraints));

  if (impl.isError()) {
    return Error(impl.error());
  }

  return OfferConstraintsFilter(std::move(*impl));
}


OfferConstraintsFilter::OfferConstraintsFilter(
    OfferConstraintsFilterImpl&& impl_)
  : impl(new OfferConstraintsFilterImpl(std::move(impl_)))
{}


OfferConstraintsFilter::OfferConstraintsFilter()
  : OfferConstraintsFilter(
        CHECK_NOTERROR(OfferConstraintsFilterImpl::create({{Bytes(0), 0}}, {})))
{}


OfferConstraintsFilter::OfferConstraintsFilter(OfferConstraintsFilter&&) =
  default;


OfferConstraintsFilter& OfferConstraintsFilter::operator=(
    OfferConstraintsFilter&&) = default;


OfferConstraintsFilter::~OfferConstraintsFilter() = default;


bool OfferConstraintsFilter::isAgentExcluded(
    const std::string& role,
    const SlaveInfo& info) const
{
  return CHECK_NOTNULL(impl)->isAgentExcluded(role, info);
}

} // namespace allocator {
} // namespace mesos {
