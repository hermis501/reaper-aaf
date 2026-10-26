// Converts between beats (quarter notes) and seconds using a tempo automation curve.
#pragma once

#include <vector>

namespace dawp {

class TempoMap
{
public:
  struct Point
  {
    double time = 0;  // in beats or seconds, see Build()
    double bpm = 120;
    bool linear = false;  // ramp linearly (in beats) towards the next point
  };

  explicit TempoMap(double bpm = 120.0);

  // Replaces the curve. 'timesInSeconds' tells whether Point::time is in seconds or beats.
  // Before the first point the tempo of the first point applies.
  void Build(std::vector<Point> points, bool timesInSeconds);

  double ToSeconds(double beats) const;
  double ToBeats(double seconds) const;
  double BpmAtBeat(double beats) const;
  bool IsLinearAtBeat(double beats) const;  // inside a ramp segment

  struct Segment
  {
    double beat, sec, bpm;
    bool linear;
  };
  const std::vector<Segment>& Segments() const { return segs_; }

private:
  int FindByBeat(double beats) const;
  int FindBySec(double seconds) const;
  double SegmentEndBpm(int i) const;

  std::vector<Segment> segs_;  // always at least one, first starts at beat 0
};

}  // namespace dawp
