# Committer's Guide

This is an attempt to capture the "etiquette" that we've informally
used with Review Board and committing.  A lot of this is obvious and
is in no way meant to be patronizing. Instead, we're just trying to be
comprehensive and pedantic.

------------------------------------------------------------------

**(1)** **Wait for a 'ship it'**. We often use the original developer or
current "custodian" of some particular code to be the reviewer and add
others to get more feedback or as an FYI. Feel free to explicitly call
out that you're adding some people just as an FYI in the
'Description'. It's worth mentioning that we often reach out directly
to one another about a particular change/feature before we even start
coding. A little context goes a long way to streamlining the review
process.

**(2)** **Resolve issues before committing**. We tend to give a 'ship it' even
when we think there are some issues that need correcting. Sometimes a
particular issue will require more discussion and sometimes we take
that discussion to IM or IRC or emails to expedite the process. It's
important, however, that we publicly capture any
collaborations/discussions that happened in those means. Making the
discussion public also helps involve others that would have liked to
get involved but weren't invited.

**(2.1)** If the reviewer and reviewee are having problems resolving a
particular "confrontational" issue then both parties should consider
asking another reviewer to participate. We're all here to build the
highest quality code possible, and we should leverage one another to
do so.

**(2.2)** When an issue is "Dropped" by the reviewee, the expectation is
that there is a reply to the issue indicating why it was dropped. A
silent "Drop" is very ambiguous.

**(2.3)** If an issue is marked as "Resolved", the expectation is that the
diff has been updated in accordance (more or less) with the reviewer's
comment. If there are significant changes, a reply to the issue with a
comment is greatly appreciated.

**(3)** **Be patient, thoughtful, and respectful** when providing (a) feedback
on reviews and (b) commenting on feedback. This is not meant to be a
sparring match. A prerequisite to being a good reviewee is
acknowledging that 'writing is hard'! The reviewee should give the
reviewer the benefit of the doubt that the reviewer has a real concern
even if they can't articulate it well. And the reviewee should be
prepared to anticipate all of the reviewers concerns and be thoughtful
about why they decided to do something a particular way (especially if
it deviates from a standard in the codebase). On the flip side, the
reviewer should not use the fact that 'writing is hard' as a crutch
for giving hasty feedback. The reviewer should invest time and energy
trying to explain their concerns clearly and carefully. It's a two-way
street!

**(4)** **Be explicit about asking for more feedback.** Feel free to update
reviews as often as you like but recognize that in many cases it's
ambiguous for reviewers when a review is back into a "ready" state so
the reviewee should feel free to ping the reviewers via email when
they're ready.

**(5)** **Follow the format of commit messages.** The three important bits are
(a) be clear and explicit in the commit message and (b) include the
link to the review and (c) use 72 character columns. See
support/apply-review.sh for committing someone else's code (it will
construct a commit message that you'll still need to edit because it
pulls in all of the 'Description' which might just be 'See summary.'
which can be omitted). Note that we don't always have a 50 character
or less summary because that restriction tends to cause people to
write poorly.

**(6)** **Never ever commit a merge.** Rebase as appropriate. Likewise, never
'force push'.

**(7)** **Don't break the build.** People use Linux and Mac OS X, be
thoughtful about doing a 'make check' on both before committing your
code. In the future we hope to have Jenkins running builds of our
reviews as well as our commits and not allowing code to be committed
until Jenkins gives it a 'ship it'. Note that if you do break the
build, the fixes are often small and inconsequential so don't worry
about going through a review cycle for that, just fix things (but
don't take that as a license to "wait until the build fails to fix
things").