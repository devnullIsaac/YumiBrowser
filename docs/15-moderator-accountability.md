# Moderator Accountability

## Who "Moderators" Are

Throughout this document, "moderator" and "administrator" refer to peers designated with moderation roles **inside a specific group registrar**, as configured by the group's owner and role policy (see [Group Registrar](05-group-registrar.md)). They are regular users of Yumi Browser whom a group has granted certain permissions within that group.

The **Yumi Browser application itself is not a moderator**. The project and its maintainers operate no central service, hold no group keys, do not observe group content, and do not moderate, remove, filter, or approve content inside any group. The project has no technical facility through which it could perform such moderation. Moderation authority originates from and is bounded by the group registrar, not from the browser software or its authors.

## Transparent Logging

When a moderator removes a member, the browser logs this event locally on every group member's machine as part of the signed audit chain. Under the default, unmodified client, the admin has no supported path to suppress the log entry on other members' machines. Every member observes that the event occurred and who was removed. If an admin starts banning members aggressively, the remaining members can see the pattern.

## Recovery Mode Is About Social Graph Continuity, Not Decision Reversal

When a member is removed from a group, that removal stands. Recovery mode does *not* override the moderator's decision, reverse the ban inside the original group, or force the removed user back into a space they were kicked out of. What recovery mode does is preserve the ability for the removed person to reach the people they used to talk to, and to make it trivially easy for those people — if they choose — to form a new group together.

The mechanics: when a user is notified of a peer's removal, they have the option to allow recovery-mode contact with the removed peer. The browser retains the last known connection information for the removed peer. If the remaining member opts in, the removed peer can reach out, tell their side, and — if enough members agree — a new group can be created that brings that social graph with them. The original group continues to exist under its current moderators. The new group exists alongside it. Nothing is reversed; something new is built.

This reframes the incentive structure around moderation. A moderator or administrator who alienates the people in their group does not lose a single kicked member — they risk losing the community. If enough members think a ban was unjust, they can collectively form a new group and leave the moderator "in the dust." This reduces the toxicity that unchecked moderation tends to produce on centralized platforms, where a kicked user is simply gone and the remaining users have no recourse short of abandoning the space entirely. In Yumi, the recourse is built in: the social graph belongs to the people, not to the moderator, and the cost of forking is near zero.

## Notification Behavior

For groups under 50 members, all members receive a notification when someone is removed, asking if they'd like to keep the door open for that user. For larger groups, users can toggle which removed members they want to keep tabs on.

This is the digital equivalent of a community splitting from a bad leader. In Yumi, the social graph lives on each user's machine, the data is distributed among peers, and creating a new group costs nothing. The power to fork is available to every member, and its presence is what disciplines moderation in the first place.
