---
title: Apache Mesos - Effective Code Reviewing
layout: documentation
---

# Effective Code Reviewing

### Preparing for Review

We've found that clear, clean, small, independent, incremental changes are much
easier to review thoroughly, and much easier to get committed. Here are some tips
to consider before sending review requests:

1. **Use up front discussion**: Surprising a reviewer is likely to lead to
   wasted effort if the overall design needs changing. Try to get a reviewer
   on the same page before sending them a review request.
2. **Think about how to break up your patch**: Did you make any changes
   that could be reviewed in independent patches? We use
   `support/post-reviews.py`, which makes it easy to create "chains" of
   reviews based on commits. Become familiar with interactive rebasing
   (`git rebase -i`) to split up commits.
3. **Provide context for the change**: Make the motivations for the
   change clear in the review request, so the reviewer is not left
   guessing. It is highly recommended to attach a JIRA issue with your
   review for additional context.
4. **Follow the [style guide](c++-style-guide.md)
   and the style of code around you**.
5. **Do a self-review of your changes before publishing**: Approach it
   from the perspective of a reviewer with no context. Is it easy to figure
   out the motivations for the change? Does it fit in with the rest of the
   code style? Are there unnecessary changes that crept in? Did you do any
   testing of the change?

### Reviewing

1. **Do high level reviewing before low-level reviewing**: Consider the
   big picture before you get into any low-level details in the code.
   For example, do you understand what motivated the change? Why the
   author chose the particular solution? Does it "feel" more complicated
   than necessary? If so, invest the time to consider if there are
   simpler approaches to the problem, or whether the scope of the work
   can be reduced. These should all be addressed before doing a
   line-by-line thorough code review, as the latter is subject to a
   lot of review back-and-forth in the face of changes to the overall
   approach.
2. **Be patient, thoughtful, and respectful**: when providing (a) feedback
   on reviews and (b) commenting on feedback. Practice
   [ego-less programming](http://blog.codinghorror.com/the-ten-commandments-of-egoless-programming/),
   this is not meant to be a sparring match! A prerequisite to being a good
   reviewee is acknowledging that 'writing is hard'! The reviewee should give
   the reviewer the benefit of the doubt that the reviewer has a real concern
   even if they can't articulate it well. And the reviewee should be prepared
   to anticipate all of the reviewers concerns and be thoughtful about why
   they decided to do something a particular way (especially if it deviates
   from a standard in the codebase). On the flip side, the reviewer should
   not use the fact that 'writing is hard' as a crutch for giving hasty
   feedback. The reviewer should invest time and energy trying to explain
   their concerns clearly and carefully. It's a two-way street!
3. **Resolve issues before committing**: we tend to give a 'ship it' even when
   we think there are some issues that need correcting. Sometimes a particular
   issue will require more discussion and sometimes we take that discussion to
   IM or IRC or emails to expedite the process. It's important, however, that
   we publicly capture any collaborations/discussions that happened in those
   means. Making the discussion public also helps involve others that would
   have liked to get involved but weren't invited.
    a. If the reviewer and reviewee are having problems resolving a particular
       "confrontational" issue then both parties should consider communicating
       directly or asking another reviewer to participate. **We're all here to
       build the highest quality code possible, and we should leverage one
       another to do so.**
    b. When an issue is "Dropped" by the reviewee, the expectation is that there
       must be a reply to the issue indicating why it was dropped. A silent "Drop"
       is discouraged.
    c. If an issue is marked as "Resolved", the expectation is that the diff has
       been updated in accordance (more or less) with the reviewer's comment. If
       there are significant changes, a reply to the issue with a comment is
       greatly appreciated.
4. **Be explicit about asking for more feedback**: feel free to update reviews
as often as you like but recognize that in many cases it's ambiguous for
reviewers when a review is back into a "ready" state so the reviewee should
feel free to ping the reviewers via email when they're ready.
5. **For reviewees, wait for a 'ship it'**: We often use the original developer
or current "custodian" of some particular code to be the reviewer and add others
to get more feedback or as an FYI. Feel free to explicitly call out that you're
adding some people just as an FYI in the 'Description'. It's worth mentioning
that we often reach out directly to one another about a particular change/feature
before we even start coding. A little context goes a long way to streamlining the
review process.
6. **For reviewers, be thorough when giving a 'ship it'**: understand that
reviewing requires a substantial investment of time and focus:
    a. You are expected to understand and support code that you're giving a 'ship it' to.
    b. You are expected to be accountable for the quality of the code you've given a 'ship it' to.
