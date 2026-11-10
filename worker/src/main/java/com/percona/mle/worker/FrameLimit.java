/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

package com.percona.mle.worker;

import com.oracle.truffle.api.exception.AbstractTruffleException;
import com.oracle.truffle.api.frame.VirtualFrame;
import com.oracle.truffle.api.instrumentation.EventContext;
import com.oracle.truffle.api.instrumentation.ExecutionEventNode;
import com.oracle.truffle.api.instrumentation.SourceSectionFilter;
import com.oracle.truffle.api.instrumentation.StandardTags;
import com.oracle.truffle.api.instrumentation.TruffleInstrument;
import com.oracle.truffle.api.nodes.Node;
import com.oracle.truffle.api.CompilerDirectives.TruffleBoundary;
import com.oracle.truffle.api.Option;
import java.net.URI;
import org.graalvm.options.OptionCategory;
import org.graalvm.options.OptionDescriptors;
import org.graalvm.options.OptionKey;
import org.graalvm.options.OptionStability;

/**
 * Oracle MLE's recursion limit: a routine may have at most 1000 frames of its own code on the
 * stack ("Maximum frame limit of 1000 exceeded. Frames on stack: 1001."). GraalJS has no such
 * limit, so this instrument counts function entries in routine sources (URI scheme mle-routine,
 * Polyglot.ROUTINE_SCHEME); the worker's helpers do not count. Enabled with the option
 * mle-frame-limit=true. Python has its own recursion limit (1000 by default).
 *
 * <p>One counter for the worker: guest code runs on the worker's main thread only. Dispatcher
 * resets it at the start of each top-level request.
 */
@TruffleInstrument.Registration(id = FrameLimit.ID, name = "MLE frame limit")
public final class FrameLimit extends TruffleInstrument {
  static final String ID = "mle-frame-limit";
  static final int LIMIT = 1000;

  @Option(name = "", help = "Limit routine frames to 1000 (Oracle MLE).", category = OptionCategory.USER,
      stability = OptionStability.STABLE)
  static final OptionKey<Boolean> ENABLED = new OptionKey<>(false);

  private static int depth;

  /** At the start of each top-level request (an unwinding may have skipped returns). */
  static void reset() {
    depth = 0;
  }

  @Override
  protected void onCreate(Env env) {
    if (!env.getOptions().get(ENABLED)) return;
    SourceSectionFilter filter = SourceSectionFilter.newBuilder()
        .tagIs(StandardTags.RootTag.class)
        // The wrapper around a body is not the routine's code (JsOracle).
        .rootNameIs(name -> !JsOracle.WRAPPER.equals(name))
        .sourceIs(s -> {
          URI uri = s.getURI();
          return Polyglot.isRoutineUri(uri);
        })
        .build();
    env.getInstrumenter().attachExecutionEventFactory(filter, FrameLimit::counter);
  }

  private static ExecutionEventNode counter(EventContext context) {
    return new ExecutionEventNode() {
      @Override
      protected void onEnter(VirtualFrame frame) {
        if (++depth > LIMIT) {
          int frames = depth--;  // onReturnExceptional does not run for a failed onEnter
          throw tooDeep(frames, this);
        }
      }

      @Override
      protected void onReturnValue(VirtualFrame frame, Object result) {
        depth--;
      }

      @Override
      protected void onReturnExceptional(VirtualFrame frame, Throwable exception) {
        depth--;
      }
    };
  }

  @TruffleBoundary
  private static Exceeded tooDeep(int frames, Node location) {
    return new Exceeded("Maximum frame limit of " + LIMIT + " exceeded. Frames on stack: " + frames + ".", location);
  }

  /** A guest error (the routine's language reports it). */
  static final class Exceeded extends AbstractTruffleException {
    private static final long serialVersionUID = 1L;

    Exceeded(String message, Node location) {
      super(message, location);
    }
  }

  @Override
  protected OptionDescriptors getOptionDescriptors() {
    return new FrameLimitOptionDescriptors();
  }
}
