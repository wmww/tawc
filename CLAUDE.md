Hi Claude, this project is Tess's Android Wayland Compositor (tawc)

## Notes
[notes.md](notes.md) contains architecture and implementation notes, and should be kept up to date with new choices and discoveries.

This is an agent-written project. Existing code/notes may be wrong in all sorts of ways. Stay vigilant, and always be prepared to surface/fix problems (even when working on something else).

## Testing
Complex code should be tested, and testing should be as general as possible (integration tests more than unit tests). An integration test means thinking about the system as a whole, and getting as much of it as practical into each test setup. Tests should only be written if they can test meaningful nontrivial properties of the components and/or how they work together. Invalid/nonuseful tests can always be improved or removed as needed.

## Iteration
Getting a good workflow for debugging/iterating is often nontrivial. For example you may need access to a real Android phone to test hardware acceleration and other features. The goal is always to enable as much autonomy as possible, without needing repeated human involvement for iteration. Feel free to ask for whatever one-time setup you need to make this possible.

## Safety
I'm letting you play with my phone, try not to fuck it up.

## Organization
Avoid junking up devices (eg delete screenshots you take when you're done with them). On the phone things should generally stay in `/data/local/arch-chroot/`, `/data/local/claude-debug` and the termux home directory
