import type { ReactNode } from "react";

export default function PageHeading({ title, copy, actions }: { title: string; copy?: string; actions?: ReactNode }) {
  return <header className="page-heading"><div><h1>{title}</h1>{copy && <p>{copy}</p>}</div>{actions && <div className="page-actions">{actions}</div>}</header>;
}
