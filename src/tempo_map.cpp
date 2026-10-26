#include "tempo_map.h"

#include <algorithm>
#include <cmath>

namespace dawp {

static const double kMinBpm = 1.0;

static double ClampBpm(double bpm) { return std::isfinite(bpm) && bpm >= kMinBpm ? bpm : 120.0; }

// Seconds needed to advance 'len' beats while the tempo ramps linearly (in beats) from t0 to t1
// over 'span' beats. len <= span.
static double RampSeconds(double t0, double t1, double span, double len)
{
  if (span <= 0 || std::fabs(t1 - t0) < 1e-9) return len * 60.0 / t0;
  double k = (t1 - t0) / span;  // bpm per beat
  return 60.0 / k * std::log((t0 + k * len) / t0);
}

// Inverse of RampSeconds: beats advanced after 'sec' seconds.
static double RampBeats(double t0, double t1, double span, double sec)
{
  if (span <= 0 || std::fabs(t1 - t0) < 1e-9) return sec * t0 / 60.0;
  double k = (t1 - t0) / span;
  return t0 / k * (std::exp(sec * k / 60.0) - 1.0);
}

TempoMap::TempoMap(double bpm) { segs_.push_back({0.0, 0.0, ClampBpm(bpm), false}); }

void TempoMap::Build(std::vector<Point> points, bool timesInSeconds)
{
  if (points.empty()) return;
  std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.time < b.time; });
  for (auto& p : points) p.bpm = ClampBpm(p.bpm);

  segs_.clear();
  if (points[0].time > 0) segs_.push_back({0.0, 0.0, points[0].bpm, false});

  for (size_t i = 0; i < points.size(); i++)
  {
    const Point& p = points[i];
    double t = std::max(0.0, p.time);
    Segment s{0, 0, p.bpm, p.linear && i + 1 < points.size()};
    if (segs_.empty())
    {
      s.beat = 0;
      s.sec = 0;
    }
    else
    {
      const Segment& prev = segs_.back();
      // The ramp target of the previous segment is this point's tempo.
      double t0 = prev.bpm, t1 = prev.linear ? p.bpm : prev.bpm;
      if (timesInSeconds)
      {
        double dsec = t - prev.sec;
        if (prev.linear)
        {
          // span in beats of a ramp from t0 to t1 lasting dsec seconds
          double span = std::fabs(t1 - t0) < 1e-9 ? dsec * t0 / 60.0 : dsec * (t1 - t0) / (60.0 * std::log(t1 / t0));
          s.beat = prev.beat + span;
        }
        else
          s.beat = prev.beat + dsec * t0 / 60.0;
        s.sec = t;
      }
      else
      {
        double span = t - prev.beat;
        s.beat = t;
        s.sec = prev.sec + RampSeconds(t0, t1, span, span);
      }
      if (s.beat <= prev.beat + 1e-12)
      {
        // Same position: the later point wins.
        segs_.back().bpm = p.bpm;
        segs_.back().linear = s.linear;
        continue;
      }
    }
    segs_.push_back(s);
  }
}

double TempoMap::SegmentEndBpm(int i) const
{
  const Segment& s = segs_[i];
  if (!s.linear || i + 1 >= (int)segs_.size()) return s.bpm;
  return segs_[i + 1].bpm;
}

int TempoMap::FindByBeat(double beats) const
{
  int lo = 0, hi = (int)segs_.size() - 1;
  while (lo < hi)
  {
    int mid = (lo + hi + 1) / 2;
    if (segs_[mid].beat <= beats) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

int TempoMap::FindBySec(double seconds) const
{
  int lo = 0, hi = (int)segs_.size() - 1;
  while (lo < hi)
  {
    int mid = (lo + hi + 1) / 2;
    if (segs_[mid].sec <= seconds) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

double TempoMap::ToSeconds(double beats) const
{
  if (!std::isfinite(beats)) return beats;
  if (beats <= 0) return beats * 60.0 / segs_[0].bpm;
  int i = FindByBeat(beats);
  const Segment& s = segs_[i];
  double len = beats - s.beat;
  if (s.linear)
  {
    double span = segs_[i + 1].beat - s.beat;
    return s.sec + RampSeconds(s.bpm, SegmentEndBpm(i), span, len);
  }
  return s.sec + len * 60.0 / s.bpm;
}

double TempoMap::ToBeats(double seconds) const
{
  if (!std::isfinite(seconds)) return seconds;
  if (seconds <= 0) return seconds * segs_[0].bpm / 60.0;
  int i = FindBySec(seconds);
  const Segment& s = segs_[i];
  double dsec = seconds - s.sec;
  if (s.linear)
  {
    double span = segs_[i + 1].beat - s.beat;
    return s.beat + RampBeats(s.bpm, SegmentEndBpm(i), span, dsec);
  }
  return s.beat + dsec * s.bpm / 60.0;
}

double TempoMap::BpmAtBeat(double beats) const
{
  int i = FindByBeat(std::max(0.0, beats));
  const Segment& s = segs_[i];
  if (!s.linear) return s.bpm;
  double span = segs_[i + 1].beat - s.beat;
  double t1 = SegmentEndBpm(i);
  return span > 0 ? s.bpm + (t1 - s.bpm) * (beats - s.beat) / span : s.bpm;
}

bool TempoMap::IsLinearAtBeat(double beats) const { return segs_[FindByBeat(std::max(0.0, beats))].linear; }

}  // namespace dawp
