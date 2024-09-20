/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, David Sansome <me@davidsansome.com>
 * Copyright 2018-2024, Jonas Kvinge <jonas@jkvinge.net>
 * Copyright 2023, Daniel Ostertag <daniel.ostertag@dakes.de>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QString>

#include "filterparser.h"
#include "filtertree.h"
#include "filterparsersearchcomparators.h"

FilterParser::FilterParser(const QString &filter_string) : filter_string_(filter_string), iter_{}, end_{} {}

FilterTree *FilterParser::parse() {

  iter_ = filter_string_.constBegin();
  end_ = filter_string_.constEnd();

  return parseOrGroup();

}

void FilterParser::advance() {

  while (iter_ != end_ && iter_->isSpace()) {
    ++iter_;
  }

}

FilterTree *FilterParser::parseOrGroup() {

  advance();
  if (iter_ == end_) return new NopFilter;

  OrFilter *group = new OrFilter;
  group->add(parseAndGroup());
  advance();
  while (checkOr()) {
    group->add(parseAndGroup());
    advance();
  }

  return group;

}

FilterTree *FilterParser::parseAndGroup() {

  advance();
  if (iter_ == end_) return new NopFilter;

  AndFilter *group = new AndFilter();
  do {
    group->add(parseSearchExpression());
    advance();
    if (iter_ != end_ && *iter_ == QLatin1Char(')')) break;
    if (checkOr(false)) {
      break;
    }
    checkAnd();  // If there's no 'AND', we'll add the term anyway...
  } while (iter_ != end_);

  return group;

}

bool FilterParser::checkAnd() {

  if (iter_ != end_) {
    if (*iter_ == QLatin1Char('A')) {
      buf_ += *iter_;
      ++iter_;
      if (iter_ != end_ && *iter_ == QLatin1Char('N')) {
        buf_ += *iter_;
        ++iter_;
        if (iter_ != end_ && *iter_ == QLatin1Char('D')) {
          buf_ += *iter_;
          ++iter_;
          if (iter_ != end_ && (iter_->isSpace() || *iter_ == QLatin1Char('-') || *iter_ == QLatin1Char('('))) {
            advance();
            buf_.clear();
            return true;
          }
        }
      }
    }
  }

  return false;

}

bool FilterParser::checkOr(const bool step_over) {

  if (!buf_.isEmpty()) {
    if (buf_ == QLatin1String("OR")) {
      if (step_over) {
        buf_.clear();
        advance();
      }
      return true;
    }
  }
  else {
    if (iter_ != end_) {
      if (*iter_ == QLatin1Char('O')) {
        buf_ += *iter_;
        ++iter_;
        if (iter_ != end_ && *iter_ == QLatin1Char('R')) {
          buf_ += *iter_;
          ++iter_;
          if (iter_ != end_ && (iter_->isSpace() || *iter_ == QLatin1Char('-') || *iter_ == QLatin1Char('('))) {
            if (step_over) {
              buf_.clear();
              advance();
            }
            return true;
          }
        }
      }
    }
  }

  return false;

}

FilterTree *FilterParser::parseSearchExpression() {

  advance();
  if (iter_ == end_) return new NopFilter;
  if (*iter_ == QLatin1Char('(')) {
    ++iter_;
    advance();
    FilterTree *tree = parseOrGroup();
    advance();
    if (iter_ != end_) {
      if (*iter_ == QLatin1Char(')')) {
        ++iter_;
      }
    }
    return tree;
  }
  else if (*iter_ == QLatin1Char('-')) {
    ++iter_;
    FilterTree *tree = parseSearchExpression();
    if (tree->type() != FilterTree::FilterType::Nop) return new NotFilter(tree);
    return tree;
  }
  else {
    return parseSearchTerm();
  }

}

FilterTree *FilterParser::parseSearchTerm() {

  QString column;
  QString prefix;
  QString value;

  bool in_quotes = false;

  for (; iter_ != end_; ++iter_) {
    if (in_quotes) {
      if (*iter_ == QLatin1Char('"')) {
        in_quotes = false;
      }
      else {
        buf_ += *iter_;
      }
    }
    else {
      if (*iter_ == QLatin1Char('"')) {
        in_quotes = true;
      }
      else if (column.isEmpty() && *iter_ == QLatin1Char(':')) {
        column = buf_.toLower();
        buf_.clear();
        prefix.clear();  // Prefix isn't allowed here - let's ignore it
      }
      else if (iter_->isSpace() || *iter_ == QLatin1Char('(') || *iter_ == QLatin1Char(')') || *iter_ == QLatin1Char('-')) {
        break;
      }
      else if (buf_.isEmpty()) {
        // We don't know whether there is a column part in this search term thus we assume the latter and just try and read a prefix
        if (prefix.isEmpty() && (*iter_ == QLatin1Char('>') || *iter_ == QLatin1Char('<') || *iter_ == QLatin1Char('=') || *iter_ == QLatin1Char('!'))) {
          prefix += *iter_;
        }
        else if (prefix != QLatin1Char('=') && *iter_ == QLatin1Char('=')) {
          prefix += *iter_;
        }
        else {
          buf_ += *iter_;
        }
      }
      else {
        buf_ += *iter_;
      }
    }
  }

  value = buf_.toLower();
  buf_.clear();

  return createSearchTermTreeNode(column, prefix, value);

}

FilterTree *FilterParser::createSearchTermTreeNode(const QString &column, const QString &prefix, const QString &value) const {

  if (value.isEmpty() && prefix != QLatin1Char('=')) {
    return new NopFilter;
  }

  FilterParserSearchTermComparator *cmp = nullptr;

  if (!column.isEmpty()) {
    if (Song::kTextSearchColumns.contains(column, Qt::CaseInsensitive)) {
      if (prefix == QLatin1Char('=') || prefix == QLatin1String("==")) {
        cmp = new FilterParserTextEqComparator(value);
      }
      else if (prefix == QLatin1String("!=") || prefix == QLatin1String("<>")) {
        cmp = new FilterParserTextNeComparator(value);
      }
      else {
        cmp = new FilterParserDefaultComparator(value);
      }
    }
    else if (Song::kIntSearchColumns.contains(column, Qt::CaseInsensitive)) {
      bool ok = false;
      int number = value.toInt(&ok);
      if (ok) {
        if (prefix == QLatin1Char('=') || prefix == QLatin1String("==")) {
          cmp = new FilterParserIntEqComparator(number);
        }
        else if (prefix == QLatin1String("!=") || prefix == QLatin1String("<>")) {
          cmp = new FilterParserIntNeComparator(number);
        }
        else if (prefix == QLatin1Char('>')) {
          cmp = new FilterParserIntGtComparator(number);
        }
        else if (prefix == QLatin1String(">=")) {
          cmp = new FilterParserIntGeComparator(number);
        }
        else if (prefix == QLatin1Char('<')) {
          cmp = new FilterParserIntLtComparator(number);
        }
        else if (prefix == QLatin1String("<=")) {
          cmp = new FilterParserIntLeComparator(number);
        }
        else {
          cmp = new FilterParserDefaultComparator(value);
        }
      }
    }
    else if (Song::kUIntSearchColumns.contains(column, Qt::CaseInsensitive)) {
      bool ok = false;
      uint number = value.toUInt(&ok);
      if (ok) {
        if (prefix == QLatin1Char('=') || prefix == QLatin1String("==")) {
          cmp = new FilterParserUIntEqComparator(number);
        }
        else if (prefix == QLatin1String("!=") || prefix == QLatin1String("<>")) {
          cmp = new FilterParserUIntNeComparator(number);
        }
        else if (prefix == QLatin1Char('>')) {
          cmp = new FilterParserUIntGtComparator(number);
        }
        else if (prefix == QLatin1String(">=")) {
          cmp = new FilterParserUIntGeComparator(number);
        }
        else if (prefix == QLatin1Char('<')) {
          cmp = new FilterParserUIntLtComparator(number);
        }
        else if (prefix == QLatin1String("<=")) {
          cmp = new FilterParserUIntLeComparator(number);
        }
        else {
          cmp = new FilterParserInt64EqComparator(number);
        }
      }
    }
    else if (Song::kInt64SearchColumns.contains(column, Qt::CaseInsensitive)) {
      qint64 number = 0;
      if (column == QLatin1String("length")) {
        number = ParseTime(value);
      }
      else {
        number = value.toLongLong();
      }
      if (prefix == QLatin1Char('=') || prefix == QLatin1String("==")) {
        cmp = new FilterParserInt64EqComparator(number);
      }
      else if (prefix == QLatin1String("!=") || prefix == QLatin1String("<>")) {
        cmp = new FilterParserInt64NeComparator(number);
      }
      else if (prefix == QLatin1Char('>')) {
        cmp = new FilterParserInt64GtComparator(number);
      }
      else if (prefix == QLatin1String(">=")) {
        cmp = new FilterParserInt64GeComparator(number);
      }
      else if (prefix == QLatin1Char('<')) {
        cmp = new FilterParserInt64LtComparator(number);
      }
      else if (prefix == QLatin1String("<=")) {
        cmp = new FilterParserInt64LeComparator(number);
      }
      else {
        cmp = new FilterParserInt64EqComparator(number);
      }
    }
    else if (Song::kFloatSearchColumns.contains(column, Qt::CaseInsensitive)) {
      const float rating = ParseRating(value);
      if (prefix == QLatin1Char('=') || prefix == QLatin1String("==")) {
        cmp = new FilterParserFloatEqComparator(rating);
      }
      else if (prefix == QLatin1String("!=") || prefix == QLatin1String("<>")) {
        cmp = new FilterParserFloatNeComparator(rating);
      }
      else if (prefix == QLatin1Char('>')) {
        cmp = new FilterParserFloatGtComparator(rating);
      }
      else if (prefix == QLatin1String(">=")) {
        cmp = new FilterParserFloatGeComparator(rating);
      }
      else if (prefix == QLatin1Char('<')) {
        cmp = new FilterParserFloatLtComparator(rating);
      }
      else if (prefix == QLatin1String("<=")) {
        cmp = new FilterParserFloatLeComparator(rating);
      }
      else {
        cmp = new FilterParserFloatEqComparator(rating);
      }
    }
  }

  if (cmp) {
    return new FilterColumnTerm(column, cmp);
  }

  return new FilterTerm(Song::kTextSearchColumns, new FilterParserDefaultComparator(value));

}

// Try and parse the string as '[[h:]m:]s' (ignoring all spaces),
// and return the number of seconds if it parses correctly.
// If not, the original string is returned.
// The 'h', 'm' and 's' components can have any length (including 0).
// A few examples:
//  "::"       is parsed to "0"
//  "1::"      is parsed to "3600"
//  "3:45"     is parsed to "225"
//  "1:165"    is parsed to "225"
//  "225"      is parsed to "225" (srsly! ^.^)
//  "2:3:4:5"  is parsed to "2:3:4:5"
//  "25m"      is parsed to "25m"

qint64 FilterParser::ParseTime(const QString &time_str) {

  qint64 seconds = 0;
  qint64 accum = 0;
  qint64 colon_count = 0;
  for (const QChar &c : time_str) {
    if (c.isDigit()) {
      accum = accum * 10LL + static_cast<qint64>(c.digitValue());
    }
    else if (c == QLatin1Char(':')) {
      seconds = seconds * 60LL + accum;
      accum = 0LL;
      ++colon_count;
      if (colon_count > 2) {
        return 0LL;
      }
    }
    else if (!c.isSpace()) {
      return 0LL;
    }
  }
  seconds = seconds * 60LL + accum;

  return seconds;

}

// Parses a rating search term to float.
//  If the rating is a number from 0-5, map it to 0-1
//  To use float values directly, the search term can be prefixed with "f" (rating:>f0.2)
//  If search string is 0, or by default, uses -1
// @param rating_str: Rating search 0-5, or "f0.2"
// @return float: rating from 0-1 or -1 if not rated.

float FilterParser::ParseRating(const QString &rating_str) {

  if (rating_str.isEmpty()) {
    return -1;
  }

  float rating = -1.0F;

  // Check if the search is a float
  if (rating_str.contains(QLatin1Char('f'), Qt::CaseInsensitive)) {
    if (rating_str.count(QLatin1Char('f'), Qt::CaseInsensitive) > 1) {
      return rating;
    }
    QString rating_float_str = rating_str;
    if (rating_str.at(0) == QLatin1Char('f') || rating_str.at(0) == QLatin1Char('F')) {
      rating_float_str = rating_float_str.remove(0, 1);
    }
    if (rating_str.right(1) == QLatin1Char('f') || rating_str.right(1) == QLatin1Char('F')) {
      rating_float_str.chop(1);
    }
    bool ok = false;
    const float rating_input = rating_float_str.toFloat(&ok);
    if (ok) {
      rating = rating_input;
    }
  }
  else {
    bool ok = false;
    const int rating_input = rating_str.toInt(&ok);
    // Is valid int from 0-5: convert to float
    if (ok && rating_input >= 0 && rating_input <= 5) {
      rating = static_cast<float>(rating_input) / 5.0F;
    }
  }

  // Songs with zero rating have -1 in the DB
  if (rating == 0) {
    rating = -1;
  }

  return rating;

}

QString FilterParser::ToolTip() {

  return QLatin1String("<html><head/><body><p>") +
         QObject::tr("Prefix a search term with a field name to limit the search to that field, e.g.:") +
         QLatin1Char(' ') +
         QLatin1String("<span style=\"font-weight:600;\">") +
         QObject::tr("artist") +
         QLatin1String(":</span><span style=\"font-style:italic;\">Strawbs</span> ") +
         QObject::tr("searches for all artists containing the word %1. ").arg(QLatin1String("Strawbs")) +
         QLatin1String("</p><p>") +

         QObject::tr("Search terms for numerical fields can be prefixed with %1 or %2 to refine the search, e.g.: ")
                     .arg(QLatin1String(" =, !=, &lt;, &gt;, &lt;="), QLatin1String("&gt;=")) +
         QLatin1String("<span style=\"font-weight:600;\">") +
         QObject::tr("rating") +
         QLatin1String("</span>") +
         QLatin1String(":>=") +
         QLatin1String("<span style=\"font-weight:italic;\">4</span>") +
         QLatin1String("</p><p>") +

         QObject::tr("Multiple search terms can also be combined with \"%1\" (default) and \"%2\", as well as grouped with parentheses. ")
                     .arg(QLatin1String("AND"), QLatin1String("OR")) +

         QLatin1String("</p><p><span style=\"font-weight:600;\">") +
         QObject::tr("Available fields") +
         QLatin1String(": ") + QLatin1String("</span><span style=\"font-style:italic;\">") +
         Song::kSearchColumns.join(QLatin1String(", ")) +
         QLatin1String("</span>.") +
         QLatin1String("</p></body></html>");

}
